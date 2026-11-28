#include "app_media.h"
#include "library.h"
#include "board.h"
#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { AGENT_PAGE_CHARS = 160, TXT_SECTION_BYTES = 8192 };

static library_book_t opened_book;
static pthread_mutex_t bookmark_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint reader_version, shelf_version;

unsigned library_shelf_version(void) { return atomic_load(&shelf_version); }

bool library_section_current(const library_section_t *section)
{
    return section->version == atomic_load(&reader_version);
}

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

static bool find_mark(const char *space, const char *key, uint64_t *value)
{
    nvs_handle_t marks;
    if (nvs_open(space, NVS_READONLY, &marks) != ESP_OK) return false;
    esp_err_t err = nvs_get_u64(marks, key, value);
    nvs_close(marks);
    return err == ESP_OK;
}

static uint64_t read_mark(const char *space, const char *key)
{
    uint64_t value = 0;
    return find_mark(space, key, &value) ? value : 0;
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
    return (book_position_t){.chapter = 1, .page = 1};
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

static bool txt_name(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len <= 4 || len >= LIBRARY_FILENAME_SIZE || strcasecmp(name + len - 4, ".txt") ||
        strpbrk(name, "/\\:*?\"<>|")) return false;
    for (size_t i = 0; i < len; ++i) if ((unsigned char)name[i] < 32) return false;
    return true;
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

static bool load_pushed_section(const char *id, uint32_t number, library_section_t *section)
{
    if (!chapter_exists(id, 1)) return false;
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

static cJSON *local_shelf_entry(const char *name)
{
    cJSON *item = shelf_entry(name);
    if (!item) return NULL;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
    if (id) {
        book_position_t saved = book_position(id);
        if (saved.exact && chapter_exists(id, saved.chapter))
            cJSON_AddNumberToObject(item, "shelf_bookmark", saved.chapter);
    } else {
        char path[sizeof(BOOK_DIR) + LIBRARY_FILENAME_SIZE], key[16];
        snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
        struct stat info;
        if (stat(path, &info) == 0 && info.st_size > 0 && (uint64_t)info.st_size <= INT32_MAX) {
            txt_key(key, name, &info);
            uint64_t saved;
            if (find_mark("pr_txt_pos", key, &saved) && saved < (uint64_t)info.st_size)
                cJSON_AddNumberToObject(item, "shelf_bookmark", saved * 100 / info.st_size);
        }
    }
    return item;
}

cJSON *library_shelf(int offset, book_cover_t *covers[LIBRARY_SHELF_PAGE_SIZE], char *error, size_t size)
{
    board_sdcard_info_t info;
    if (board_sdcard_mount(&info) != ESP_OK)
        return app_error(error, size, "SD 卡不可用或正被占用");
    cJSON *out = media_list(BOOK_DIR, offset, shelf_name, local_shelf_entry, error, size);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(out, "items");
    for (int i = 0; i < cJSON_GetArraySize(items) && i < LIBRARY_SHELF_PAGE_SIZE; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "file"));
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
        char name[24];
        if (id) snprintf(name, sizeof(name), "b_%s.json", id);
        if (file || id) covers[i] = book_cover_load(file ? file : name);
    }
    if (board_sdcard_unmount() != ESP_OK) {
        cJSON_Delete(out);
        return app_error(error, size, "SD 卡卸载失败");
    }
    if (!out) return app_error(error, size, "无法读取书目");
    return out;
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

static cJSON *read_book_page(const library_book_t *book, const cJSON *args, char *error, size_t size)
{
    int limit = book->local ? INT32_MAX / TXT_SECTION_BYTES + 1 : 999;
    int chapter = app_number(args, "chapter", 1, limit, 1);
    library_section_t section = {0};
    if (chapter < 1) return app_error(error, size, "Chapter outside book");
    if (book->local) {
        if (!load_txt_section(book->name, chapter, &section, error, size)) {
            free(section.text);
            return NULL;
        }
    } else if (!load_pushed_section(book->name, chapter, &section)) {
        return app_error(error, size, "Book missing or invalid");
    }
    unsigned chars = characters_before(section.text, section.length);
    int pages = (chars + AGENT_PAGE_CHARS - 1) / AGENT_PAGE_CHARS;
    int page = app_number(args, "page", 1, pages, 1);
    if (page < 1) {
        free(section.text);
        return app_error(error, size, "Page outside book");
    }
    char *start = section.text + byte_at_character(section.text, (page - 1) * AGENT_PAGE_CHARS);
    start[byte_at_character(start, AGENT_PAGE_CHARS)] = 0;
    cJSON *out = cJSON_CreateObject();
    bool ok = out && cJSON_AddStringToObject(out, book->local ? "file" : "id", book->name) &&
              cJSON_AddStringToObject(out, "title", section.title) &&
              cJSON_AddStringToObject(out, "text", start) &&
              cJSON_AddNumberToObject(out, "page", page) && cJSON_AddNumberToObject(out, "pages", pages) &&
              cJSON_AddNumberToObject(out, "chapter", chapter) && cJSON_AddBoolToObject(out, "next_chapter", section.next);
    if (ok && !book->local) {
        book_position_t saved = book_position(book->name);
        ok = cJSON_AddNumberToObject(out, "bookmark_chapter", saved.chapter) &&
             cJSON_AddNumberToObject(out, "bookmark_page", saved.page);
    }
    free(section.text);
    if (!ok) { cJSON_Delete(out); return app_error(error, size, "Cannot allocate book page"); }
    return out;
}

// FatFS accepts short aliases and case-insensitive lookups. Resolve the exact
// name returned by list before using a remote filename for a read or deletion.
static bool listed_txt(const char *name)
{
    DIR *dir = opendir(BOOK_DIR);
    if (!dir) return false;
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_DIR && !strcmp(entry->d_name, name)) { found = true; break; }
    }
    closedir(dir);
    return found;
}

static bool forget_book(const library_book_t *book, const char *key)
{
    pthread_mutex_lock(&bookmark_lock);
    if (book->local == opened_book.local && !strcmp(book->name, opened_book.name)) {
        opened_book.name[0] = 0;
        atomic_fetch_add(&reader_version, 1);
    }
    nvs_handle_t marks;
    esp_err_t err = nvs_open(book->local ? "pr_txt_pos" : "pr_read_pos", NVS_READWRITE, &marks);
    if (err == ESP_OK) {
        err = nvs_erase_key(marks, key);
        if (err == ESP_OK) err = nvs_commit(marks);
        else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        nvs_close(marks);
    }
    pthread_mutex_unlock(&bookmark_lock);
    return err == ESP_OK;
}

static bool clear_chapters(const char *id)
{
    DIR *dir = opendir(BOOK_DIR);
    if (!dir) return false;
    char prefix[16];
    size_t length = snprintf(prefix, sizeof(prefix), "b_%s_", id);
    uint8_t present[125] = {0}; // One bit for each possible chapter on the reader worker's stack.
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) break;
        const char *name = entry->d_name;
        if (entry->d_type == DT_DIR || strlen(name) != length + 7 || strncasecmp(name, prefix, length)) continue;
        const char *digits = name + length;
        if (strspn(digits, "0123456789") != 3 || strcasecmp(digits + 3, ".seg")) continue;
        int number = atoi(digits);
        if (number >= 2) present[number / 8] |= 1u << (number % 8);
    }
    bool ok = !errno;
    closedir(dir);
    for (int number = 2; ok && number <= 999; ++number) {
        if (!(present[number / 8] & (1u << (number % 8)))) continue;
        char path[96];
        chapter_path(path, id, number);
        ok = unlink(path) == 0;
    }
    return ok;
}

static cJSON *manage_book(const cJSON *args, bool *changed, char *error, size_t size)
{
    const char *action = app_text(args, "action", 16);
    if (!action) return app_error(error, size, "Missing library action");
    if (!strcmp(action, "list"))
        return media_list(BOOK_DIR, app_number(args, "offset", 0, 65535, 0), shelf_name, shelf_entry, error, size);

    const char *id = app_text(args, "id", 12);
    const cJSON *file_arg = cJSON_GetObjectItemCaseSensitive(args, "file");
    const char *file = cJSON_GetStringValue(file_arg);
    library_book_t book = {.local = file_arg != NULL};
    if (file_arg) {
        if (cJSON_GetObjectItemCaseSensitive(args, "id") || !txt_name(file))
            return app_error(error, size, "Specify only an exact TXT filename");
        if (!listed_txt(file)) return app_error(error, size, "TXT file not found");
        strlcpy(book.name, file, sizeof(book.name));
    } else {
        if (!book_id(id)) return app_error(error, size, "Invalid book ID");
        strlcpy(book.name, id, sizeof(book.name));
    }
    if (!strcmp(action, "read")) return read_book_page(&book, args, error, size);

    struct stat info;
    if (book.local) {
        if (strcmp(action, "delete")) return app_error(error, size, "Unknown TXT action");
        char path[sizeof(BOOK_DIR) + LIBRARY_FILENAME_SIZE], key[16];
        snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, file);
        if (stat(path, &info) || !S_ISREG(info.st_mode)) return app_error(error, size, "TXT file not found");
        txt_key(key, file, &info);
        if (!forget_book(&book, key) || unlink(path)) return app_error(error, size, "Cannot remove TXT file");
        *changed = true;
        return app_result("deleted");
    }

    char path[96], marker[96];
    chapter_path(path, id, 1);
    snprintf(marker, sizeof(marker), "%s/b_%s.del", BOOK_DIR, id);
    bool deleting = stat(marker, &info) == 0;
    if (!deleting && errno != ENOENT) return app_error(error, size, "Cannot check book deletion");
    if (deleting && (!S_ISREG(info.st_mode) || chapter_exists(id, 1)))
        return app_error(error, size, "Conflicting book deletion marker");
    if (!strcmp(action, "delete") && !deleting) {
        if (stat(path, &info) || !S_ISREG(info.st_mode)) return app_error(error, size, "Book not found");
        // Hide the book in a single rename; its marker survives interrupted cleanup.
        if (rename(path, marker)) return app_error(error, size, "Cannot remove book");
        deleting = true;
        *changed = true;
    }
    if (deleting) {
        if (strcmp(action, "delete") && strcmp(action, "add")) return app_error(error, size, "Book is being deleted");
        if (!forget_book(&book, id) || !clear_chapters(id) || unlink(marker))
            return app_error(error, size, "Book deletion incomplete; retry delete");
    }
    if (!strcmp(action, "delete")) return app_result("deleted");
    if (!strcmp(action, "append")) {
        int chapter = app_number(args, "chapter", 2, 999, -1);
        if (chapter < 2 || !chapter_exists(id, 1) || !chapter_exists(id, chapter - 1))
            return app_error(error, size, "Previous chapter required");
        chapter_path(path, id, chapter);
    } else if (strcmp(action, "add")) return app_error(error, size, "Unknown library action");
    cJSON *out = add_book(path, args, error, size);
    if (out) *changed = true;
    return out;
}

cJSON *library_call(const cJSON *args, char *error, size_t size)
{
    board_sdcard_info_t info;
    if (board_sdcard_mount(&info) != ESP_OK) return app_error(error, size, "SD card busy or unavailable");
    bool changed = false;
    cJSON *out = manage_book(args, &changed, error, size);
    esp_err_t err = board_sdcard_unmount();
    if (changed) atomic_fetch_add(&shelf_version, 1);
    if (err != ESP_OK) {
        cJSON_Delete(out);
        return app_error(error, size, "SD card unmount failed");
    }
    return out;
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
    if (ok) {
        pthread_mutex_lock(&bookmark_lock);
        if (opened_book.local != book->local || strcmp(opened_book.name, book->name)) {
            opened_book = *book;
            atomic_fetch_add(&reader_version, 1);
        }
        section->version = atomic_load(&reader_version);
        pthread_mutex_unlock(&bookmark_lock);
    }
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
    pthread_mutex_lock(&bookmark_lock);
    bool ok = true;
    if (library_section_current(section)) {
        if (book->local) ok = write_mark("pr_txt_pos", section->bookmark_key, section->start + offset);
        else {
            unsigned page = characters_before(section->text, offset) / AGENT_PAGE_CHARS + 1;
            uint64_t position = ((uint64_t)section->number << 32) | (page << 16) | offset;
            ok = write_mark("pr_read_pos", book->name, position);
        }
    }
    pthread_mutex_unlock(&bookmark_lock);
    return ok;
}
