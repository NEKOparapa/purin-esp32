#include "reader_screen.h"
#include "board.h"
#include "eink_ui.h"
#include "library.h"
#include "reader_layout.h"
#include "terminal_tools.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    READER_FONT_SIZE = 24,
    LINE_SPACE = 7,
    STATUS_HEIGHT = 36,
    INFO_HEIGHT = 48,
    NAV_HEIGHT = 54,
    BODY_GAP = 10,
};

typedef enum { LOAD_SHELF, OPEN_SECTION, LAST_PAGE } reader_action_t;

typedef struct {
    reader_action_t action;
    unsigned generation;
    uint32_t number;
    library_book_t book;
    int width, lines;
} reader_job_t;

typedef struct {
    library_section_t section;
    reader_layout_t layout;
} reader_document_t;

typedef struct {
    reader_job_t job;
    cJSON *shelf;
    reader_document_t document;
    char error[128];
} reader_result_t;

extern const uint8_t chat_sc_start[] asm("_binary_chat_sc_ttf_start");
extern const uint8_t chat_sc_end[] asm("_binary_chat_sc_ttf_end");

static lv_obj_t *screen, *back_screen, *shelf_bar, *page_bar, *shelf_panel, *page_panel;
static lv_obj_t *status, *shelf_entries, *remote_label, *shelf_index, *page_title, *page_text, *progress;
static lv_obj_t *shelf_prev, *shelf_next, *page_prev, *page_next, *chapter_prev, *chapter_next;
static const lv_font_t *reading_font;
static QueueHandle_t jobs, results;
static reader_document_t document;
static library_book_t selected, books[LIBRARY_SHELF_PAGE_SIZE];
static unsigned generation;
static uint32_t page, shelf_offset;
static int text_width, text_height, text_lines;
static int key_sample, key_stable;
static bool key_ready, pending, reading, shelf_loaded, shelf_more;

static const lv_font_t *create_reader_font(void)
{
    lv_font_t *font = lv_tiny_ttf_create_data(chat_sc_start, chat_sc_end - chat_sc_start, READER_FONT_SIZE);
    if (font) font->fallback = LV_FONT_DEFAULT;
    return font ? font : LV_FONT_DEFAULT;
}

static void release_document(reader_document_t *doc)
{
    free(doc->section.text);
    free(doc->layout.starts);
    *doc = (reader_document_t){0};
}

static void reader_worker(void *unused)
{
    (void)unused;
    // A separate font instance keeps Tiny TTF's mutable glyph cache off the UI task.
    const lv_font_t *metrics_font = create_reader_font();
    reader_job_t job;
    while (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
        reader_result_t result = {.job = job};
        if (job.action == LOAD_SHELF) {
            result.shelf = library_shelf(job.number, result.error, sizeof(result.error));
        } else if (library_section_load(&job.book, job.number, &result.document.section,
                                         result.error, sizeof(result.error))) {
            library_section_t *section = &result.document.section;
            if (!reader_layout_build(&result.document.layout, section->text, section->length,
                                     metrics_font, job.width, job.lines)) {
                release_document(&result.document);
                strlcpy(result.error, "无法排版这段正文", sizeof(result.error));
            }
        }
        xQueueSend(results, &result, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}

static void start_worker(void)
{
    if (jobs) return;
    jobs = xQueueCreate(1, sizeof(reader_job_t));
    results = xQueueCreate(1, sizeof(reader_result_t));
    if (jobs && results && xTaskCreate(reader_worker, "reader", 6144, NULL, 1, NULL) == pdPASS) return;
    if (jobs) vQueueDelete(jobs);
    if (results) vQueueDelete(results);
    jobs = NULL;
    results = NULL;
}

static void show(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void enable(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void update_controls(void)
{
    enable(shelf_prev, !pending && shelf_offset > 0);
    enable(shelf_next, !pending && shelf_more);
    enable(page_prev, !pending && reading && (page > 0 || document.section.previous));
    enable(page_next, !pending && reading && (page + 1 < document.layout.count || document.section.next));
    enable(chapter_prev, !pending && reading && document.section.previous);
    enable(chapter_next, !pending && reading && document.section.next);
}

static void request(reader_action_t action, uint32_t number, const library_book_t *book)
{
    if (pending) return;
    if (!jobs) {
        lv_label_set_text(status, "阅读任务不可用");
        return;
    }
    reader_job_t job = {.action = action, .generation = ++generation, .number = number,
                        .width = text_width, .lines = text_lines};
    if (book) job.book = *book;
    xQueueOverwrite(jobs, &job);
    pending = true;
    update_controls();
    lv_label_set_text(status, action == LOAD_SHELF ? "正在读取书目" : "正在打开正文");
}

static void update_remote(void)
{
    lv_label_set_text(remote_label, (device_apps_mask() & (1u << APP_LIBRARY)) ? "远程已开启" : "远程已关闭");
}

static void toggle_remote(lv_event_t *event)
{
    (void)event;
    esp_err_t err = device_apps_toggle(APP_LIBRARY);
    update_remote();
    if (!pending) lv_label_set_text(status, err != ESP_OK ? "设置保存失败" : terminal_tools_enabled() ?
                                    "" : "远程调用需配置 HTTPS 服务与设备凭据");
}

static void show_view(bool open_book)
{
    reading = open_book;
    show(page_bar, reading);
    show(page_panel, reading);
    show(shelf_bar, !reading);
    show(shelf_panel, !reading);
    update_controls();
}

static void go_back(lv_event_t *event)
{
    (void)event;
    ++generation;
    pending = false;
    release_document(&document);
    lv_label_set_text(status, "");
    if (reading) {
        lv_label_set_text(page_text, "");
        show_view(false);
        if (!shelf_loaded) request(LOAD_SHELF, 0, NULL);
    } else {
        lv_screen_load(back_screen);
    }
}

static void browse(int direction)
{
    if (!direction) request(LOAD_SHELF, 0, NULL);
    else if (direction < 0 && shelf_offset > 0)
        request(LOAD_SHELF, shelf_offset - LIBRARY_SHELF_PAGE_SIZE, NULL);
    else if (direction > 0 && shelf_more)
        request(LOAD_SHELF, shelf_offset + LIBRARY_SHELF_PAGE_SIZE, NULL);
}

static void browse_clicked(lv_event_t *event)
{
    browse((intptr_t)lv_event_get_user_data(event));
}

static void select_book(lv_event_t *event)
{
    unsigned i = (uintptr_t)lv_event_get_user_data(event);
    request(OPEN_SECTION, 0, &books[i]);
}

static void display_page(uint32_t index, const char *text)
{
    page = index;
    lv_label_set_text(page_text, text);
    library_section_t *section = &document.section;
    char info[64];
    if (selected.local) {
        uint32_t end = section->start + document.layout.starts[index + 1];
        unsigned percent = (uint64_t)end * 100 / section->total;
        snprintf(info, sizeof(info), "阅读进度  %u%%", percent);
    } else {
        snprintf(info, sizeof(info), "第 %u 章 · %u / %u 页", (unsigned)section->number,
                 (unsigned)(index + 1), (unsigned)document.layout.count);
    }
    lv_label_set_text(progress, info);
    show_view(true);
    bool saved = library_save_position(&selected, section, document.layout.starts[index]);
    lv_label_set_text(status, saved ? "" : "书签保存失败");
}

static void turn_page(int direction)
{
    if (pending) return;
    if (!reading) {
        browse(direction);
        return;
    }
    if (direction < 0 && page == 0) {
        if (document.section.previous) request(LAST_PAGE, document.section.number - 1, &selected);
    } else if (direction > 0 && page + 1 == document.layout.count) {
        if (document.section.next) request(OPEN_SECTION, document.section.number + 1, &selected);
    } else {
        uint32_t index = direction < 0 ? page - 1 : page + 1;
        char *text = reader_layout_text(&document.layout, document.section.text, index);
        if (!text) lv_label_set_text(status, "内存不足");
        else display_page(index, text);
        free(text);
    }
}

static void turn_clicked(lv_event_t *event)
{
    turn_page((intptr_t)lv_event_get_user_data(event));
}

static void chapter_clicked(lv_event_t *event)
{
    int direction = (intptr_t)lv_event_get_user_data(event);
    if (direction < 0 && document.section.previous)
        request(OPEN_SECTION, document.section.number - 1, &selected);
    else if (direction > 0 && document.section.next)
        request(OPEN_SECTION, document.section.number + 1, &selected);
}

static void page_touch(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    if (lv_event_get_code(event) == LV_EVENT_GESTURE) {
        lv_dir_t direction = lv_indev_get_gesture_dir(input);
        if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
            lv_indev_wait_release(input);
            turn_page(direction == LV_DIR_LEFT ? 1 : -1);
        }
    } else {
        lv_point_t point;
        lv_indev_get_point(input, &point);
        int width = lv_obj_get_width(screen);
        if (point.x < width / 3) turn_page(-1);
        else if (point.x >= 2 * width / 3) turn_page(1);
    }
}

static lv_obj_t *column(lv_obj_t *parent, int gap)
{
    lv_obj_t *obj = lv_obj_create(parent);
    eink_style_clean_box(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj, gap, 0);
    return obj;
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    eink_style_clean_box(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, LV_PCT(100), height);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 8, 0);
    return obj;
}

static lv_obj_t *control(lv_obj_t *parent, const char *text, int width, int height,
                         lv_event_cb_t callback, intptr_t value)
{
    lv_obj_t *button = eink_outline_button(parent, text, eink_small_font(), height,
                                           callback, (void *)value);
    lv_obj_set_width(button, width);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    return button;
}

static lv_obj_t *center_label(lv_obj_t *parent)
{
    lv_obj_t *label = eink_label(parent, "", eink_small_font(), LV_TEXT_ALIGN_CENTER);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    return label;
}

static void create_screen(void)
{
    reading_font = create_reader_font();
    screen = lv_obj_create(NULL);
    eink_style_screen(screen);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *slot;
    shelf_bar = eink_top_bar_font(screen, "阅读书架", eink_heading_font(), go_back, &slot);
    remote_label = lv_obj_get_child(control(slot, "", 128, 42, toggle_remote, 0), 0);
    page_bar = eink_top_bar_font(screen, "", eink_heading_font(), go_back, NULL);
    page_title = lv_obj_get_child(page_bar, 1);
    lv_label_set_long_mode(page_title, LV_LABEL_LONG_MODE_DOTS);

    shelf_panel = column(screen, BODY_GAP);
    lv_obj_set_flex_grow(shelf_panel, 1);
    lv_obj_set_style_pad_all(shelf_panel, EINK_PAD_X, 0);
    lv_obj_t *shelf_tools = row(shelf_panel, INFO_HEIGHT);
    control(shelf_tools, "刷新书架", 120, 42, browse_clicked, 0);
    shelf_index = center_label(shelf_tools);
    shelf_entries = column(shelf_panel, 12);
    lv_obj_set_flex_grow(shelf_entries, 1);
    lv_obj_t *shelf_nav = row(shelf_panel, NAV_HEIGHT);
    lv_obj_set_flex_align(shelf_nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    shelf_prev = control(shelf_nav, "上一组", 120, 48, browse_clicked, -1);
    shelf_next = control(shelf_nav, "下一组", 120, 48, browse_clicked, 1);

    page_panel = column(screen, BODY_GAP);
    lv_obj_set_flex_grow(page_panel, 1);
    lv_obj_set_style_pad_hor(page_panel, EINK_PAD_X, 0);
    lv_obj_set_style_pad_ver(page_panel, EINK_PAD_X, 0);
    lv_display_t *display = lv_display_get_default();
    text_width = lv_display_get_horizontal_resolution(display) - 2 * EINK_PAD_X;
    text_height = lv_display_get_vertical_resolution(display) - EINK_TOP_BAR_H - STATUS_HEIGHT -
                  INFO_HEIGHT - NAV_HEIGHT - 2 * EINK_PAD_X - 2 * BODY_GAP;
    text_lines = (text_height + LINE_SPACE) /
                 (lv_font_get_line_height(reading_font) + LINE_SPACE);
    page_text = eink_label(page_panel, "", reading_font, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(page_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(page_text, text_width, text_height);
    lv_obj_set_style_text_line_space(page_text, LINE_SPACE, 0);
    lv_obj_add_flag(page_text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(page_text, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(page_text, page_touch, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(page_text, page_touch, LV_EVENT_GESTURE, NULL);
    lv_obj_t *info = row(page_panel, INFO_HEIGHT);
    chapter_prev = control(info, "上一章", 84, 42, chapter_clicked, -1);
    progress = center_label(info);
    chapter_next = control(info, "下一章", 84, 42, chapter_clicked, 1);
    lv_obj_t *nav = row(page_panel, NAV_HEIGHT);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    page_prev = control(nav, "上一页", 120, 48, turn_clicked, -1);
    page_next = control(nav, "下一页", 120, 48, turn_clicked, 1);

    status = eink_label(screen, "", eink_small_font(), LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(status, LV_PCT(100), STATUS_HEIGHT);
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_CLIP);
}

static void render_shelf(const reader_result_t *result)
{
    lv_obj_clean(shelf_entries);
    shelf_loaded = true;
    shelf_offset = result->job.number;
    shelf_more = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result->shelf, "has_more"));
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(result->shelf, "items");
    int count = cJSON_GetArraySize(items);
    if (!count) {
        lv_obj_t *empty = eink_label(shelf_entries, "将 TXT 复制到 SD 卡 Wintermute/Library\n或让 Agent 推送一篇文章。",
                                     eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(empty, LV_PCT(100));
    }
    for (int i = 0; i < count && i < LIBRARY_SHELF_PAGE_SIZE; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "file"));
        const char *name = file ? file : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "title"));
        if (!name || !title) continue;
        books[i].local = file != NULL;
        strlcpy(books[i].name, name, sizeof(books[i].name));
        lv_obj_t *book = eink_outline_button(shelf_entries, title, eink_body_font(), 100,
                                             select_book, (void *)(intptr_t)i);
        lv_obj_remove_flag(book, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(book, LV_PCT(100));
        lv_obj_t *heading = lv_obj_get_child(book, 0);
        lv_obj_set_width(heading, LV_PCT(88));
        lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 22, 16);
        lv_obj_t *kind = eink_label(book, file ? "TXT" : "Agent", eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_align(kind, LV_ALIGN_BOTTOM_LEFT, 22, -14);
    }
    char info[32];
    snprintf(info, sizeof(info), "第 %u 组", (unsigned)(shelf_offset / LIBRARY_SHELF_PAGE_SIZE + 1));
    lv_label_set_text(shelf_index, count || shelf_offset ? info : "");
    lv_label_set_text(status, "");
}

static void render_section(reader_result_t *result)
{
    reader_document_t *next = &result->document;
    uint32_t index = 0;
    if (result->job.action == LAST_PAGE) index = next->layout.count - 1;
    else {
        while (index + 1 < next->layout.count && next->layout.starts[index + 1] <= next->section.resume)
            ++index;
    }
    char *text = reader_layout_text(&next->layout, next->section.text, index);
    if (!text) {
        lv_label_set_text(status, "内存不足");
        return;
    }
    release_document(&document);
    document = *next;
    *next = (reader_document_t){0};
    selected = result->job.book;
    show(chapter_prev, !selected.local);
    show(chapter_next, !selected.local);
    lv_label_set_text(page_title, document.section.title);
    display_page(index, text);
    free(text);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    bool active = lv_screen_active() == screen;
    reader_result_t result;
    if (results && xQueueReceive(results, &result, 0) == pdTRUE) {
        if (active && result.job.generation == generation) {
            pending = false;
            if (result.shelf) render_shelf(&result);
            else if (result.document.layout.count) render_section(&result);
            else lv_label_set_text(status, result.error);
            update_controls();
        }
        cJSON_Delete(result.shelf);
        release_document(&result.document);
    }
    if (!active) return;
    int direction;
    if (board_volume_key(&direction) == ESP_OK) {
        if (direction == key_sample && (!key_ready || direction != key_stable)) {
            key_stable = direction;
            if (key_ready && direction) turn_page(direction);
            if (!direction) key_ready = true;
        }
        key_sample = direction;
    }
}

void reader_open(lv_obj_t *back, const char *book_id)
{
    if (!screen) {
        create_screen();
        lv_timer_create(tick, 40, NULL);
    }
    start_worker();
    ++generation;
    back_screen = back;
    pending = shelf_loaded = shelf_more = key_ready = false;
    key_sample = key_stable = 0;
    shelf_offset = page = 0;
    release_document(&document);
    lv_obj_clean(shelf_entries);
    lv_label_set_text(shelf_index, "");
    lv_label_set_text(status, "");
    update_remote();
    show_view(false);
    lv_screen_load(screen);
    if (book_id) {
        library_book_t book = {0};
        strlcpy(book.name, book_id, sizeof(book.name));
        request(OPEN_SECTION, 0, &book);
    } else request(LOAD_SHELF, 0, NULL);
}
