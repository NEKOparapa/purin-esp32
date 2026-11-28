#include "schedule_screen.h"
#include "day_schedule.h"
#include "eink_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *screen, *home_screen, *date, *rows, *status, *navigation, *prev, *next, *number, *refresh;
static QueueHandle_t completed;
static time_t visible_day;
static bool pending, needs_first_page;
enum { CACHED_CURSORS = 8 };
static struct { unsigned index; char cursor[DAY_SCHEDULE_CURSOR]; } recent[CACHED_CURSORS];
static unsigned current_index;
static char next_cursor[DAY_SCHEDULE_CURSOR];

static void set_status(const char *message)
{
    lv_label_set_text_static(status, message);
    lv_obj_set_flag(status, LV_OBJ_FLAG_HIDDEN, !*message);
}

static void read_schedule(void *arg)
{
    day_schedule_page_t *page = arg;
    day_schedule_fetch(page);
    xQueueSend(completed, &page, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void update_navigation(void)
{
    lv_obj_set_flag(navigation, LV_OBJ_FLAG_HIDDEN, current_index == 0 && !*next_cursor);
    lv_obj_set_state(refresh, LV_STATE_DISABLED, pending);
    lv_obj_set_state(prev, LV_STATE_DISABLED, pending || current_index == 0);
    lv_obj_set_state(next, LV_STATE_DISABLED, pending || !*next_cursor);
}

static void request_page(unsigned target)
{
    if (pending) return;
    if (!completed) completed = xQueueCreate(1, sizeof(day_schedule_page_t *));
    day_schedule_page_t *page = completed ? calloc(1, sizeof(*page)) : NULL;
    if (page) {
        page->start = visible_day;
        page->target = target;
        if (target > current_index && *next_cursor) {
            page->index = current_index + 1;
            strcpy(page->cursor, next_cursor);
        } else {
            for (unsigned i = 0; i < CACHED_CURSORS; ++i) {
                if (recent[i].index > page->index && recent[i].index <= target) {
                    page->index = recent[i].index;
                    strcpy(page->cursor, recent[i].cursor);
                }
            }
        }
        pending = xTaskCreate(read_schedule, "day_schedule", 6144, page, 1, NULL) == pdPASS;
        if (!pending) free(page);
    }
    set_status(pending ? (visible_day ? "正在读取今日日程…" : "正在核对设备日期…") : "内存不足，请刷新重试");
    update_navigation();
}

static void load_clicked(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target_obj(event);
    if (button == prev && current_index) request_page(current_index - 1);
    else if (button == next && *next_cursor) request_page(current_index + 1);
    else if (button == refresh) request_page(0);
}

static void go_home(lv_event_t *event)
{
    (void)event;
    lv_screen_load(home_screen);
}

static void set_day(time_t start)
{
    visible_day = start;
    memset(recent, 0, sizeof(recent));
    current_index = 0;
    next_cursor[0] = 0;
    lv_obj_clean(rows);
    update_navigation();
    struct tm local;
    static const char *const weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    if (start && localtime_r(&start, &local))
        lv_label_set_text_fmt(date, "%d 月 %d 日　%s", local.tm_mon + 1, local.tm_mday, weekdays[local.tm_wday]);
    else lv_label_set_text_static(date, "日期待同步");
    set_status("");
    needs_first_page = true;
}

static void append_row(const day_schedule_row_t *row)
{
    lv_obj_t *entry = lv_obj_create(rows);
    eink_style_clean_box(entry);
    lv_obj_remove_flag(entry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(entry, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(entry, 12, 0);
    lv_obj_set_style_pad_ver(entry, 5, 0);
    lv_obj_t *clock = eink_label(entry, row->at, eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(clock, 72);

    lv_obj_t *copy = lv_obj_create(entry);
    eink_style_clean_box(copy);
    lv_obj_remove_flag(copy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(copy, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(copy, 1);
    lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(copy, 5, 0);
    lv_obj_t *title = eink_label(copy, row->title, eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(title, LV_PCT(100));
    if (*row->preview) {
        lv_obj_t *label = eink_label(copy, row->preview, eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_line_space(label, 3, 0);
    }
}

static void show_group(const day_schedule_page_t *page, bool notices)
{
    lv_obj_t *title = eink_label(rows, notices ? "当日通知" : "当日安排", eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_pad_top(title, 8, 0);
    eink_divider(rows, LV_PCT(100));
    bool any = false;
    for (unsigned i = 0; i < page->count; ++i) {
        if (page->rows[i].notice == notices) {
            append_row(&page->rows[i]);
            any = true;
        }
    }
    if (!any) {
        bool only_page = !page->index && !*page->next_cursor;
        const char *empty = notices ? (only_page ? "当日暂无通知" : "本页暂无通知")
                                    : (only_page ? "当日暂无安排" : "本页暂无安排");
        eink_label(rows, empty, eink_small_font(), LV_TEXT_ALIGN_LEFT);
    }
}

static void show_page(const day_schedule_page_t *page)
{
    // A refreshed first page invalidates all previously discovered positions.
    if (page->index == 0) memset(recent, 0, sizeof(recent));
    current_index = page->index;
    unsigned slot = current_index % CACHED_CURSORS;
    recent[slot].index = current_index;
    strcpy(recent[slot].cursor, page->cursor);
    strcpy(next_cursor, page->next_cursor);
    lv_obj_clean(rows);
    lv_obj_scroll_to_y(rows, 0, LV_ANIM_OFF);
    show_group(page, true);
    show_group(page, false);
    lv_label_set_text_fmt(number, "第 %u 页", current_index + 1);
    set_status("");
}

static void sync_screen(lv_timer_t *timer)
{
    (void)timer;
    if (lv_screen_active() != screen) {
        day_schedule_page_t *unused;
        if (pending && xQueueReceive(completed, &unused, 0) == pdTRUE) {
            pending = false;
            free(unused);
        }
        return;
    }
    time_t end;
    time_t today = day_schedule_bounds(time(NULL), &end);
    if (today != visible_day) set_day(today);
    day_schedule_page_t *page;
    if (pending && xQueueReceive(completed, &page, 0) == pdTRUE) {
        pending = false;
        if (page->start == visible_day) {
            needs_first_page = false;
            if (page->error) set_status(page->error);
            else show_page(page);
        } else needs_first_page = true;
        update_navigation();
        free(page);
    }
    if (needs_first_page && !pending) {
        needs_first_page = false;
        request_page(0);
    }
}

void schedule_screen_open(lv_obj_t *home)
{
    home_screen = home;
    if (!screen) {
        screen = lv_obj_create(NULL);
        eink_style_screen(screen);
        lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *slot;
        eink_top_bar_font(screen, "日程", eink_heading_font(), go_home, &slot);
        refresh = eink_refresh_button(slot, load_clicked);
        date = eink_label(screen, "", eink_heading_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_pad_left(date, EINK_PAD_X, 0);
        lv_obj_set_style_pad_top(date, 16, 0);
        status = eink_label(screen, "", eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(status, LV_PCT(100));
        lv_obj_set_style_pad_hor(status, EINK_PAD_X, 0);
        rows = lv_obj_create(screen);
        eink_style_clean_box(rows);
        lv_obj_set_size(rows, LV_PCT(100), 0);
        lv_obj_set_flex_grow(rows, 1);
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(rows, LV_DIR_VER);
        lv_obj_set_style_pad_all(rows, EINK_PAD_X, 0);
        lv_obj_set_style_pad_row(rows, 8, 0);
        navigation = lv_obj_create(screen);
        eink_style_clean_box(navigation);
        lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(navigation, LV_PCT(100), 64);
        lv_obj_set_style_pad_hor(navigation, EINK_PAD_X, 0);
        lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(navigation, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        prev = eink_outline_button(navigation, "上一页", eink_small_font(), 44, load_clicked, NULL);
        number = eink_label(navigation, "", eink_small_font(), LV_TEXT_ALIGN_CENTER);
        next = eink_outline_button(navigation, "下一页", eink_small_font(), 44, load_clicked, NULL);
        set_day(0);
        lv_timer_create(sync_screen, 500, NULL);
    }
    needs_first_page = true;
    lv_screen_load(screen);
    sync_screen(NULL);
}
