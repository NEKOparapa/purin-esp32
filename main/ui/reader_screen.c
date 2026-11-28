#include "reader_screen.h"
#include "board.h"
#include "eink_ui.h"
#include "home_graphics.h"
#include "library.h"
#include "reader_layout.h"
#include "reading_progress.h"
#include "src/misc/cache/instance/lv_image_cache.h"
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
    NAV_HEIGHT = 54,
    BODY_GAP = 10,
    RUNNING_HEAD_HEIGHT = 38,
    PAGE_PAD_Y = 16,
    PAGE_MARGIN = 28,
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
    book_cover_t *covers[LIBRARY_SHELF_PAGE_SIZE];
    reader_document_t document;
    char error[128];
} reader_result_t;

extern const uint8_t ui_sc_start[] asm("_binary_ui_sc_ttf_start");
extern const uint8_t ui_sc_end[] asm("_binary_ui_sc_ttf_end");

static lv_obj_t *screen, *back_screen, *shelf_bar, *shelf_panel, *page_panel, *tools_panel;
static lv_obj_t *status, *shelf_entries, *shelf_count, *shelf_index, *page_title, *page_text, *progress;
static lv_obj_t *page_feedback;
static lv_obj_t *shelf_refresh, *shelf_nav;
static lv_obj_t *shelf_prev, *shelf_next, *page_prev, *page_next, *chapter_prev, *chapter_next;
static const lv_font_t *reading_font;
static QueueHandle_t jobs, results;
static reader_document_t document;
static library_book_t selected, books[LIBRARY_SHELF_PAGE_SIZE];
static lv_image_dsc_t cover_images[LIBRARY_SHELF_PAGE_SIZE];
static unsigned generation, shelf_version;
static uint32_t page, shelf_offset;
static int text_width, text_height, text_lines;
static int key_sample, key_stable;
static bool key_ready, pending, reading, shelf_loaded, shelf_more;

static void show_shelf_message(const char *title, const char *detail);

static const lv_font_t *create_reader_font(void)
{
    lv_font_t *font = lv_tiny_ttf_create_data(ui_sc_start, ui_sc_end - ui_sc_start, READER_FONT_SIZE);
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
            result.shelf = library_shelf(job.number, result.covers, result.error, sizeof(result.error));
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

static void set_status(const char *message)
{
    lv_label_set_text(status, message);
    show(status, !reading && *message);
    lv_label_set_text(page_feedback, message);
    show(page_feedback, *message != 0);
    show(progress, *message == 0);
}

static void update_controls(void)
{
    show(shelf_nav, shelf_offset > 0 || shelf_more);
    enable(shelf_refresh, !pending);
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
        if (action == LOAD_SHELF && !shelf_loaded) {
            show_shelf_message("无法读取书架", "阅读任务不可用");
        } else {
            set_status("阅读任务不可用");
        }
        return;
    }
    reader_job_t job = {.action = action, .generation = ++generation, .number = number,
                        .width = text_width, .lines = text_lines};
    if (book) job.book = *book;
    if (action == LOAD_SHELF) shelf_version = library_shelf_version();
    xQueueOverwrite(jobs, &job);
    pending = true;
    update_controls();
    if (action == LOAD_SHELF && !shelf_loaded) {
        show_shelf_message("正在读取书架…", NULL);
        set_status("");
    } else {
        set_status(action == LOAD_SHELF ? "正在刷新书架…" : "正在打开正文");
    }
}

static void show_view(bool open_book)
{
    reading = open_book;
    show(tools_panel, false);
    show(page_panel, reading);
    show(status, !reading && *lv_label_get_text(status));
    show(shelf_bar, !reading);
    show(shelf_panel, !reading);
    update_controls();
}

static bool save_reading_update(void)
{
    if (!reading || page >= document.layout.count || !library_section_current(&document.section)) return true;
    return reading_progress_submit(&selected, &document.section,
                                   document.layout.starts[page + 1], page + 1, document.layout.count);
}

static void on_screen_unload(lv_event_t *event)
{
    (void)event;
    save_reading_update();
}

static void go_back(lv_event_t *event)
{
    (void)event;
    ++generation;
    pending = false;
    bool saved = save_reading_update();
    release_document(&document);
    set_status(saved ? "" : "阅读进度上报记录保存失败");
    if (reading) {
        lv_label_set_text(page_text, "");
        show_view(false);
        if (saved || !shelf_loaded) request(LOAD_SHELF, shelf_offset, NULL);
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
    set_status(saved ? "" : "书签保存失败");
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
        if (!text) set_status("内存不足");
        else display_page(index, text);
        free(text);
    }
}

static void turn_clicked(lv_event_t *event)
{
    show(tools_panel, false);
    turn_page((intptr_t)lv_event_get_user_data(event));
}

static void chapter_clicked(lv_event_t *event)
{
    show(tools_panel, false);
    int direction = (intptr_t)lv_event_get_user_data(event);
    if (direction < 0 && document.section.previous)
        request(OPEN_SECTION, document.section.number - 1, &selected);
    else if (direction > 0 && document.section.next)
        request(OPEN_SECTION, document.section.number + 1, &selected);
}

static void toggle_tools(lv_event_t *event)
{
    (void)event;
    show(tools_panel, lv_obj_has_flag(tools_panel, LV_OBJ_FLAG_HIDDEN));
}

static void page_touch(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    if (!lv_obj_has_flag(tools_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_indev_wait_release(input);
        show(tools_panel, false);
        return;
    }
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
        else toggle_tools(event);
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

static void show_shelf_message(const char *title, const char *detail)
{
    show(shelf_count, false);
    lv_obj_clean(shelf_entries);
    lv_obj_t *message = column(shelf_entries, 12);
    lv_obj_set_size(message, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_align(message, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    eink_icon_reading(message, false);
    eink_label(message, title, eink_heading_font(), LV_TEXT_ALIGN_CENTER);
    if (detail && detail[0]) {
        lv_obj_t *text = eink_label(message, detail, eink_small_font(), LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(text, LV_PCT(100));
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    }
}

static void create_screen(void)
{
    reading_font = create_reader_font();
    screen = lv_obj_create(NULL);
    lv_obj_add_event_cb(screen, on_screen_unload, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    eink_style_screen(screen);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *slot;
    shelf_bar = eink_top_bar_font(screen, "书架", eink_heading_font(), go_back, &slot);
    shelf_refresh = eink_refresh_button(slot, browse_clicked);

    shelf_panel = column(screen, BODY_GAP);
    lv_obj_set_flex_grow(shelf_panel, 1);
    lv_obj_set_style_pad_all(shelf_panel, EINK_PAD_X, 0);
    shelf_count = eink_label(shelf_panel, "", eink_small_font(), LV_TEXT_ALIGN_LEFT);
    show(shelf_count, false);
    shelf_entries = column(shelf_panel, 16);
    lv_obj_set_flex_grow(shelf_entries, 1);
    lv_obj_set_flex_flow(shelf_entries, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(shelf_entries, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    shelf_nav = row(shelf_panel, NAV_HEIGHT);
    lv_obj_set_flex_align(shelf_nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    shelf_prev = control(shelf_nav, "上一组", 120, 48, browse_clicked, -1);
    shelf_index = center_label(shelf_nav);
    shelf_next = control(shelf_nav, "下一组", 120, 48, browse_clicked, 1);

    page_panel = column(screen, BODY_GAP);
    lv_obj_set_flex_grow(page_panel, 1);
    lv_obj_set_style_pad_hor(page_panel, PAGE_MARGIN, 0);
    lv_obj_set_style_pad_ver(page_panel, PAGE_PAD_Y, 0);
    lv_obj_t *running_head = row(page_panel, RUNNING_HEAD_HEIGHT);
    lv_obj_set_style_border_side(running_head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(running_head, lv_color_black(), 0);
    lv_obj_set_style_border_width(running_head, 1, 0);
    page_title = eink_label(running_head, "", eink_small_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(page_title, LV_PCT(100));
    lv_label_set_long_mode(page_title, LV_LABEL_LONG_MODE_DOTS);
    lv_display_t *display = lv_display_get_default();
    text_width = lv_display_get_horizontal_resolution(display) - 2 * PAGE_MARGIN;
    text_height = lv_display_get_vertical_resolution(display) - RUNNING_HEAD_HEIGHT - NAV_HEIGHT
                  - 2 * PAGE_PAD_Y - 2 * BODY_GAP;
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
    lv_obj_t *footer = row(page_panel, NAV_HEIGHT);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(footer, lv_color_black(), 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    progress = center_label(footer);
    lv_obj_set_style_text_align(progress, LV_TEXT_ALIGN_LEFT, 0);
    page_feedback = center_label(footer);
    lv_obj_set_style_text_align(page_feedback, LV_TEXT_ALIGN_LEFT, 0);
    show(page_feedback, false);
    control(footer, "工具", 76, 44, toggle_tools, 0);

    status = eink_label(screen, "", eink_small_font(), LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(status, LV_PCT(100), STATUS_HEIGHT);
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_CLIP);

    // Overlay the current page so opening tools leaves its text and bookmark in place.
    tools_panel = column(screen, 12);
    lv_obj_add_flag(tools_panel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_height(tools_panel, LV_SIZE_CONTENT);
    lv_obj_align(tools_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(tools_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tools_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(tools_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(tools_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(tools_panel, 2, 0);
    lv_obj_set_style_pad_all(tools_panel, EINK_PAD_X, 0);
    lv_obj_t *actions = row(tools_panel, NAV_HEIGHT);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    control(actions, "返回书架", 124, 48, go_back, 0);
    control(actions, "收起", 80, 48, toggle_tools, 0);
    lv_obj_t *navigation = row(tools_panel, NAV_HEIGHT);
    page_prev = control(navigation, "上一页", 0, 48, turn_clicked, -1);
    page_next = control(navigation, "下一页", 0, 48, turn_clicked, 1);
    chapter_prev = control(navigation, "上一章", 0, 48, chapter_clicked, -1);
    chapter_next = control(navigation, "下一章", 0, 48, chapter_clicked, 1);
    lv_obj_set_flex_grow(page_prev, 1);
    lv_obj_set_flex_grow(page_next, 1);
    lv_obj_set_flex_grow(chapter_prev, 1);
    lv_obj_set_flex_grow(chapter_next, 1);
    show(tools_panel, false);
}

static void release_cover_image(lv_event_t *event)
{
    lv_image_dsc_t *image = lv_event_get_user_data(event);
    lv_image_cache_drop(image);
    free((void *)image->data);
    image->data = NULL;
}

static void display_cover(lv_obj_t *book, unsigned slot, book_cover_t *cover)
{
    lv_obj_t *frame = lv_obj_create(book);
    eink_style_clean_box(frame);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(frame, COVER_WIDTH + 4, COVER_HEIGHT + 4);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    if (cover) {
        lv_image_dsc_t *image = &cover_images[slot];
        *image = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I1,
                       .w = COVER_WIDTH, .h = COVER_HEIGHT, .stride = COVER_WIDTH / 8},
            .data_size = sizeof(*cover), .data = (const uint8_t *)cover,
        };
        lv_obj_t *art = lv_image_create(frame);
        lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(art, image);
        lv_obj_center(art);
        lv_obj_add_event_cb(art, release_cover_image, LV_EVENT_DELETE, image);
    } else {
        lv_obj_t *spine = eink_divider(frame, 1);
        lv_obj_set_height(spine, LV_PCT(100));
        lv_obj_align(spine, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_t *glyph = home_glyph(frame, HOME_BOOK, 46);
        lv_obj_align(glyph, LV_ALIGN_CENTER, 4, 0);
    }
}

static void render_shelf(reader_result_t *result)
{
    shelf_loaded = true;
    shelf_offset = result->job.number;
    shelf_more = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result->shelf, "has_more"));
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(result->shelf, "items");
    int count = cJSON_GetArraySize(items);
    if (count) {
        if (shelf_more || shelf_offset)
            lv_label_set_text_fmt(shelf_count, "第 %u–%u 本", (unsigned)shelf_offset + 1, (unsigned)shelf_offset + count);
        else lv_label_set_text_fmt(shelf_count, "共 %u 本", (unsigned)count);
        show(shelf_count, true);
    }
    if (!count) {
        show_shelf_message("书架为空", "将 TXT 复制到 SD 卡 Purin/Library，或让 Agent 推送书籍。");
    } else {
        lv_obj_clean(shelf_entries);
    }
    for (int i = 0; i < count && i < LIBRARY_SHELF_PAGE_SIZE; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "file"));
        const char *name = file ? file : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "title"));
        if (!name || !title) continue;
        books[i].local = file != NULL;
        strlcpy(books[i].name, name, sizeof(books[i].name));
        lv_obj_t *book = lv_button_create(shelf_entries);
        eink_style_clean_box(book);
        lv_obj_remove_flag(book, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(book, LV_PCT(48), LV_SIZE_CONTENT);
        lv_obj_set_style_text_decor(book, LV_TEXT_DECOR_UNDERLINE, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(book, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(book, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(book, 5, 0);
        lv_obj_add_event_cb(book, select_book, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        display_cover(book, i, result->covers[i]);
        result->covers[i] = NULL;
        lv_obj_t *heading = eink_label(book, title, eink_body_font(), LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(heading, LV_PCT(100));
        lv_obj_set_style_max_height(heading, 2 * lv_font_get_line_height(eink_body_font()), 0);
        lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_t *meta = eink_label(book, file ? "本机 TXT" : "推送书籍", eink_small_font(), LV_TEXT_ALIGN_CENTER);
        const cJSON *bookmark = cJSON_GetObjectItemCaseSensitive(item, "shelf_bookmark");
        if (cJSON_IsNumber(bookmark)) {
            if (file) lv_label_set_text_fmt(meta, "书签 %d%%", bookmark->valueint);
            else lv_label_set_text_fmt(meta, "书签在第 %d 章", bookmark->valueint);
        }
    }
    char info[32];
    snprintf(info, sizeof(info), "第 %u 组", (unsigned)(shelf_offset / LIBRARY_SHELF_PAGE_SIZE + 1));
    lv_label_set_text(shelf_index, count || shelf_offset ? info : "");
    set_status("");
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
        set_status("内存不足");
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
        // The upload picker can cover the reader while its current request completes.
        // Leaving the reader or opening a different book advances the generation.
        if (result.job.generation == generation) {
            pending = false;
            if (result.shelf) render_shelf(&result);
            else if (result.document.layout.count) render_section(&result);
            else if (result.job.action == LOAD_SHELF && !shelf_loaded) {
                show_shelf_message("无法读取书架", result.error);
                set_status("");
            } else set_status(result.error);
            update_controls();
        }
        cJSON_Delete(result.shelf);
        for (unsigned i = 0; i < LIBRARY_SHELF_PAGE_SIZE; ++i) free(result.covers[i]);
        release_document(&result.document);
    }
    if (!active) return;
    if (!pending && reading && !library_section_current(&document.section)) {
        release_document(&document);
        lv_label_set_text(page_text, "");
        show_view(false);
        request(LOAD_SHELF, 0, NULL);
    } else if (!pending && !reading && shelf_version != library_shelf_version()) {
        request(LOAD_SHELF, 0, NULL);
    }
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
    show(shelf_count, false);
    ++generation;
    back_screen = back;
    pending = shelf_loaded = shelf_more = key_ready = false;
    key_sample = key_stable = 0;
    shelf_offset = page = 0;
    release_document(&document);
    lv_obj_clean(shelf_entries);
    lv_label_set_text(shelf_index, "");
    set_status("");
    show_view(false);
    lv_screen_load(screen);
    if (book_id) {
        library_book_t book = {0};
        strlcpy(book.name, book_id, sizeof(book.name));
        request(OPEN_SECTION, 0, &book);
    } else request(LOAD_SHELF, 0, NULL);
}
