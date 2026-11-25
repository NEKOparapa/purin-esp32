#include "app_media.h"
#include "library.h"
#include "board.h"
#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { AGENT_PAGE_CHARS = 160, TXT_SECTION_BYTES = 8192 };

cJSON *media_list(const char *dir, int offset, media_name_fn valid, media_entry_fn entry,
                  char *error, size_t size)
{
    if (offset < 0) return app_error(error, size, "Invalid offset");
    cJSON *out = cJSON_CreateObject();
    cJSON *items = out ? cJSON_AddArrayToObject(out, "items") : NULL;
    if (!items) { cJSON_Delete(out); return NULL; }
    DIR *d = opendir(dir);
    if (!d && errno != ENOENT) { cJSON_Delete(out); return app_error(error, size, "Cannot open directory"); }
    int seen = 0;
    bool more = false, ok = true;
    if (d) {
        struct dirent *e;
        while (true) {
            errno = 0;
            e = readdir(d);
            if (!e) { ok = errno == 0; break; }
            if (e->d_type == DT_DIR || !valid(e->d_name)) continue;
            if (seen++ < offset) continue;
            if (cJSON_GetArraySize(items) == LIBRARY_SHELF_PAGE_SIZE) { more = true; break; }
            cJSON *item = entry(e->d_name);
            if (!item || !cJSON_AddItemToArray(items, item)) {
                cJSON_Delete(item); ok = false; break;
            }
        }
        closedir(d);
    }
    if (!ok) { cJSON_Delete(out); return app_error(error, size, "Cannot read directory entry"); }
    cJSON_AddBoolToObject(out, "has_more", more);
    return out;
}

static bool book_id(const char *id)
{
    return id && *id && strlen(id) <= 12 && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(id);
}

static bool book_name(const char *name)
{
    size_t len = strlen(name);
    if (len < 8 || len > 19 || strncmp(name, "b_", 2) || strcmp(name + len - 5, ".json")) return false;
    char id[13] = {0};
    memcpy(id, name + 2, len - 7);
    return book_id(id);
}

static cJSON *load_book(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    cJSON *book = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long length = ftell(file);
        if (length > 0 && length <= 16000 && !fseek(file, 0, SEEK_SET)) {
            char *text = malloc((size_t)length + 1);
            if (text && fread(text, 1, length, file) == (size_t)length) {
                text[length] = 0;
                book = cJSON_Parse(text);
            }
            free(text);
        }
    }
    fclose(file);
    return book;
}

static cJSON *book_entry(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
    cJSON *book = load_book(path);
    const char *title = app_text(book, "title", 48);
    char id[13] = {0};
    memcpy(id, name + 2, strlen(name) - 7);
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "id", id);
        cJSON_AddStringToObject(item, "title", title ? title : id);
    }
    cJSON_Delete(book);
    return item;
}

static bool ensure_directory(const char *dir) { return mkdir(dir, 0775) == 0 || errno == EEXIST; }

static cJSON *add_book(const char *path, const cJSON *args, char *error, size_t size)
{
    const char *title = app_text(args, "title", 48), *text = app_text(args, "text", 2500);
    if (!title || !text) return app_error(error, size, "Invalid book title or text");
    if (!ensure_directory(APP_STORAGE_ROOT) || !ensure_directory(BOOK_DIR))
        return app_error(error, size, "Cannot create library");
    struct stat info;
    if (!stat(path, &info)) {
        cJSON *book = load_book(path);
        const char *old_title = app_text(book, "title", 48), *old_text = app_text(book, "text", 2500);
        bool same = old_title && old_text && !strcmp(title, old_title) && !strcmp(text, old_text);
        cJSON_Delete(book);
        return same ? app_result("saved") : app_error(error, size, "Book chapter already has different content");
    }
    if (errno != ENOENT) return app_error(error, size, "Cannot check book ID");
    char *encoded = cJSON_PrintUnformatted(args);
    if (!encoded) return NULL;
    const char *temp = BOOK_DIR "/pending.tmp";
    FILE *file = fopen(temp, "wb");
    bool ok = file && fputs(encoded, file) >= 0 && !fflush(file) && !fsync(fileno(file));
    cJSON_free(encoded);
    if (file && fclose(file)) ok = false;
    if (!ok || rename(temp, path)) {
        unlink(temp);
        return app_error(error, size, "Book was not saved");
    }
    return app_result("saved");
}

static void chapter_path(char path[96], const char *id, int chapter)
{
    if (chapter == 1) snprintf(path, 96, "%s/b_%s.json", BOOK_DIR, id);
    else snprintf(path, 96, "%s/b_%s_%03d.seg", BOOK_DIR, id, chapter);
}

typedef struct {
    unsigned chapter, page, byte;
    bool exact;
} book_position_t;

static uint64_t read_mark(const char *space, const char *key)
{
    nvs_handle_t marks;
    uint64_t value = 0;
    if (nvs_open(space, NVS_READONLY, &marks) == ESP_OK) {
        nvs_get_u64(marks, key, &value);
        nvs_close(marks);
    }
    return value;
}

static bool write_mark(const char *space, const char *key, uint64_t value)
{
    nvs_handle_t marks;
    if (nvs_open(space, NVS_READWRITE, &marks) != ESP_OK) return false;
    uint64_t old;
    bool ok = nvs_get_u64(marks, key, &old) == ESP_OK && old == value;
    if (!ok) ok = nvs_set_u64(marks, key, value) == ESP_OK && nvs_commit(marks) == ESP_OK;
    nvs_close(marks);
    return ok;
}

static book_position_t book_position(const char *id)
{
    uint64_t exact = read_mark("pr_read_pos", id);
    if (exact)
        return (book_position_t){exact >> 32, (exact >> 16) & 0xffff, exact & 0xffff, true};
    nvs_handle_t marks;
    uint32_t legacy = 0x10001;
    if (nvs_open("pr_bookmarks", NVS_READONLY, &marks) == ESP_OK) {
        nvs_get_u32(marks, id, &legacy);
        nvs_close(marks);
    }
    return (book_position_t){.chapter = legacy >> 16, .page = legacy & 0xffff};
}

static bool continuation(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

static uint32_t byte_at_character(const char *text, unsigned character)
{
    const char *end = text;
    while (*end) {
        if (character-- == 0) break;
        do { ++end; } while (continuation(*end));
    }
    return end - text;
}

static unsigned characters_before(const char *text, uint32_t bytes)
{
    unsigned count = 0;
    for (uint32_t i = 0; i < bytes; ++i)
        if (!continuation(text[i])) ++count;
    return count;
}

static bool chapter_exists(const char *id, unsigned chapter)
{
    char path[96];
    struct stat info;
    if (chapter < 1 || chapter > 999) return false;
    chapter_path(path, id, chapter);
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static cJSON *read_page(const char *id, const cJSON *args, char *error, size_t size)
{
    int chapter = app_number(args, "chapter", 1, 999, 1);
    char path[96];
    chapter_path(path, id, chapter);
    cJSON *book = chapter > 0 && chapter <= 999 ? load_book(path) : NULL;
    const char *text = app_text(book, "text", 2500), *title = app_text(book, "title", 48);
    if (!text || !title) {
        cJSON_Delete(book);
        return app_error(error, size, "Book missing or invalid");
    }
    unsigned chars = characters_before(text, strlen(text));
    int pages = (chars + AGENT_PAGE_CHARS - 1) / AGENT_PAGE_CHARS;
    int page = app_number(args, "page", 1, pages, 1);
    cJSON *out = NULL;
    if (page < 1) {
        app_error(error, size, "Page outside book");
    } else {
        const char *start = text + byte_at_character(text, (page - 1) * AGENT_PAGE_CHARS);
        const char *end = start + byte_at_character(start, AGENT_PAGE_CHARS);
        char chunk[AGENT_PAGE_CHARS * 4 + 1];
        memcpy(chunk, start, end - start);
        chunk[end - start] = 0;
        out = cJSON_CreateObject();
        if (out) {
            cJSON_AddStringToObject(out, "id", id);
            cJSON_AddStringToObject(out, "title", title);
            cJSON_AddStringToObject(out, "text", chunk);
            cJSON_AddNumberToObject(out, "page", page);
            cJSON_AddNumberToObject(out, "pages", pages);
            book_position_t saved = book_position(id);
            cJSON_AddNumberToObject(out, "chapter", chapter);
            cJSON_AddBoolToObject(out, "next_chapter", chapter_exists(id, chapter + 1));
            cJSON_AddNumberToObject(out, "bookmark_chapter", saved.chapter);
            cJSON_AddNumberToObject(out, "bookmark_page", saved.page);
        }
    }
    cJSON_Delete(book);
    return out;
}

cJSON *library_call(const cJSON *args, char *error, size_t size)
{
    const char *action = app_text(args, "action", 16);
    if (!action) return app_error(error, size, "Missing library action");
    board_sdcard_info_t info;
    if (board_sdcard_mount(&info) != ESP_OK) return app_error(error, size, "SD card busy or unavailable");
    cJSON *out = NULL;
    if (!strcmp(action, "list")) {
        out = media_list(BOOK_DIR, app_number(args, "offset", 0, 65535, 0), book_name, book_entry, error, size);
    } else {
        const char *id = app_text(args, "id", 12);
        if (!book_id(id)) app_error(error, size, "Invalid book ID");
        else {
            char path[96];
            chapter_path(path, id, 1);
            if (!strcmp(action, "add")) out = add_book(path, args, error, size);
            else if (!strcmp(action, "read")) out = read_page(id, args, error, size);
            else if (!strcmp(action, "append")) {
                int chapter = app_number(args, "chapter", 2, 999, -1);
                struct stat info;
                chapter_path(path, id, chapter - 1);
                if (chapter < 2 || stat(path, &info) != 0) app_error(error, size, "Previous chapter required");
                else { chapter_path(path, id, chapter); out = add_book(path, args, error, size); }
            }
            else app_error(error, size, "Unknown library action");
        }
    }
    if (board_sdcard_unmount() != ESP_OK) {
        cJSON_Delete(out);
        return app_error(error, size, "SD card unmount failed");
    }
    return out;
}

static bool txt_name(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && len < LIBRARY_FILENAME_SIZE && !strcasecmp(name + len - 4, ".txt") &&
           !strchr(name, '/') && !strchr(name, '\\');
}

static bool shelf_name(const char *name)
{
    return book_name(name) || txt_name(name);
}

static cJSON *shelf_entry(const char *name)
{
    if (!txt_name(name)) return book_entry(name);
    char title[LIBRARY_FILENAME_SIZE];
    strlcpy(title, name, strlen(name) - 3);
    cJSON *item = cJSON_CreateObject();
    if (item && (!cJSON_AddStringToObject(item, "file", name) ||
                 !cJSON_AddStringToObject(item, "title", title))) {
        cJSON_Delete(item);
        item = NULL;
    }
    return item;
}

cJSON *library_shelf(int offset, char *error, size_t size)
{
    board_sdcard_info_t info;
    if (board_sdcard_mount(&info) != ESP_OK)
        return app_error(error, size, "SD 卡不可用或正被占用");
    cJSON *out = media_list(BOOK_DIR, offset, shelf_name, shelf_entry, error, size);
    if (board_sdcard_unmount() != ESP_OK) {
        cJSON_Delete(out);
        return app_error(error, size, "SD 卡卸载失败");
    }
    if (!out) return app_error(error, size, "无法读取书目");
    return out;
}

static bool load_pushed_section(const char *id, uint32_t number, library_section_t *section)
{
    book_position_t saved = {0};
    if (!number) {
        saved = book_position(id);
        number = chapter_exists(id, saved.chapter) ? saved.chapter : 1;
    }
    if (number > 999) return false;
    char path[96];
    chapter_path(path, id, number);
    cJSON *chapter = load_book(path);
    const char *text = app_text(chapter, "text", 2500), *title = app_text(chapter, "title", 48);
    if (text && title) {
        section->text = strdup(text);
        section->length = strlen(text);
        strlcpy(section->title, title, sizeof(section->title));
        if (number == saved.chapter && saved.page > 0) {
            uint32_t pos = saved.exact ? saved.byte :
                           byte_at_character(text, (saved.page - 1) * AGENT_PAGE_CHARS);
            if (pos < section->length && !continuation(text[pos])) section->resume = pos;
        }
    }
    cJSON_Delete(chapter);
    if (!section->text) return false;
    section->number = number;
    section->previous = chapter_exists(id, number - 1);
    section->next = chapter_exists(id, number + 1);
    return true;
}

static void txt_key(char key[16], const char *name, const struct stat *info)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        hash = (hash ^ c) * UINT64_C(1099511628211);
    }
    hash = (hash ^ (uint64_t)info->st_size) * UINT64_C(1099511628211);
    hash = (hash ^ (uint64_t)info->st_mtime) * UINT64_C(1099511628211);
    snprintf(key, 16, "%015" PRIx64, hash & UINT64_C(0xfffffffffffffff));
}

// Adjacent sections use the same boundary. Move past UTF-8 tails and keep CRLF together.
static bool txt_boundary(FILE *file, uint32_t total, uint32_t *position)
{
    uint32_t pos = *position;
    if (pos >= total) return true;
    if (fseek(file, pos ? pos - 1 : 0, SEEK_SET)) return false;
    if (!pos) {
        unsigned char bom[3];
        if (fread(bom, 1, sizeof(bom), file) == sizeof(bom) &&
            !memcmp(bom, "\xef\xbb\xbf", sizeof(bom))) *position = sizeof(bom);
        return !ferror(file);
    }
    int previous = fgetc(file), code = fgetc(file);
    if (previous == EOF || code == EOF) return false;
    if (previous == '\r' && code == '\n') ++pos;
    else {
        while (code != EOF && continuation(code) && pos - *position < 3) {
            ++pos;
            code = fgetc(file);
        }
        if (ferror(file) || (code != EOF && continuation(code))) return false;
    }
    *position = pos;
    return true;
}

static bool readable_utf8(const unsigned char *text, uint32_t size)
{
    for (uint32_t i = 0; i < size;) {
        unsigned char c = text[i++];
        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') return false;
            continue;
        }
        if (c < 0xc2 || c > 0xf4 || i == size) return false;
        unsigned char next = text[i];
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) return false;
        int tails = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        while (tails-- > 0)
            if (i == size || !continuation(text[i++])) return false;
    }
    return true;
}

static bool load_txt_section(const char *name, uint32_t number, library_section_t *section,
                             char *error, size_t size)
{
    char path[sizeof(BOOK_DIR) + LIBRARY_FILENAME_SIZE];
    strlcpy(error, "无法读取 TXT 文件", size);
    snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
    struct stat info;
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size > INT32_MAX) return false;
    uint32_t total = info.st_size;
    if (!total) {
        strlcpy(error, "TXT 文件为空", size);
        return false;
    }
    txt_key(section->bookmark_key, name, &info);
    uint64_t saved = !number ? read_mark("pr_txt_pos", section->bookmark_key) : 0;
    if (saved >= total) saved = 0;
    if (!number) number = saved / TXT_SECTION_BYTES + 1;
    if (number > (total - 1) / TXT_SECTION_BYTES + 1) return false;
    uint32_t start = (number - 1) * TXT_SECTION_BYTES;
    uint32_t end = start + TXT_SECTION_BYTES;
    if (end > total) end = total;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = txt_boundary(file, total, &start) && txt_boundary(file, total, &end) && end > start;
    if (!ok && start == total) strlcpy(error, "TXT 文件为空", size);
    if (ok) {
        section->text = malloc(end - start + 1);
        ok = section->text && !fseek(file, start, SEEK_SET) &&
             fread(section->text, 1, end - start, file) == end - start;
    }
    fclose(file);
    if (!ok) return false;
    if (!readable_utf8((const unsigned char *)section->text, end - start)) {
        strlcpy(error, "请将 TXT 文件保存为 UTF-8 编码", size);
        return false;
    }
    section->text[end - start] = '\0';
    strlcpy(section->title, name, strlen(name) - 3);
    section->number = number;
    section->start = start;
    section->length = end - start;
    section->total = total;
    section->previous = number > 1;
    section->next = end < total;
    if (saved >= start && saved < end && !continuation(section->text[saved - start]))
        section->resume = saved - start;
    return true;
}

bool library_section_load(const library_book_t *book, uint32_t number,
                          library_section_t *section, char *error, size_t size)
{
    *section = (library_section_t){0};
    if (!(book->local ? txt_name(book->name) : book_id(book->name))) {
        strlcpy(error, "书目无效", size);
        return false;
    }
    board_sdcard_info_t info;
    if (board_sdcard_mount(&info) != ESP_OK) {
        strlcpy(error, "SD 卡不可用或正被占用", size);
        return false;
    }
    strlcpy(error, "章节不存在或已损坏", size);
    bool ok = book->local ? load_txt_section(book->name, number, section, error, size) :
                           load_pushed_section(book->name, number, section);
    if (board_sdcard_unmount() != ESP_OK) {
        strlcpy(error, "SD 卡卸载失败", size);
        ok = false;
    }
    if (!ok) {
        free(section->text);
        section->text = NULL;
    }
    return ok;
}

bool library_save_position(const library_book_t *book, const library_section_t *section,
                           uint32_t offset)
{
    if (offset >= section->length) return false;
    if (book->local)
        return write_mark("pr_txt_pos", section->bookmark_key, section->start + offset);
    unsigned page = characters_before(section->text, offset) / AGENT_PAGE_CHARS + 1;
    uint64_t position = ((uint64_t)section->number << 32) | (page << 16) | offset;
    return write_mark("pr_read_pos", book->name, position);
}
