#include "home_response.h"
#include "device_apps_screen.h"
#include "eink_ui.h"
#include "home_graphics.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

extern const uint8_t chat_sc_start[] asm("_binary_chat_sc_ttf_start");
extern const uint8_t chat_sc_end[] asm("_binary_chat_sc_ttf_end");
static lv_obj_t *home, *standby, *dial, *date_row, *clock_label, *date_label, *weekday_label;
static lv_obj_t *sync_message;
static int dial_minute = -1;
static lv_obj_t *event_view, *category, *event_time, *headline, *detail, *open_button;
static lv_obj_t *context, *context_label, *query_label, *progress, *glyphs[APP_COUNT];
static const lv_font_t *clock_font, *hour_font, *title_font;
static device_app_t target = APP_COUNT, context_target = APP_COUNT;
static char book_id[13];
// LVGL replaces the end of overflowing labels with dots in its private text buffer.
static char last_title[sizeof(((prompt_card_t *)0)->title)];
static char last_body[sizeof(((prompt_card_t *)0)->body)];

static void show(lv_obj_t *obj, bool visible)
{
    if (visible == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_text(lv_obj_t *label, const char *value)
{
    if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    eink_style_clean_box(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(o, LV_PCT(100));
    return o;
}

static lv_obj_t *line_label(lv_obj_t *parent, const lv_font_t *font, int max_height)
{
    lv_obj_t *o = eink_label(parent, "", font, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_style_max_height(o, max_height, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    return o;
}

static void open_response(lv_event_t *event)
{
    bool secondary = lv_event_get_user_data(event) != NULL;
    device_app_t app = secondary ? context_target : target;
    if (app == APP_LIBRARY && !secondary && book_id[0]) device_book_open(book_id, home);
    else if (app < APP_COUNT) device_app_open(app, home);
}

static const lv_font_t *display_font(int px, const lv_font_t *fallback)
{
    lv_font_t *font = lv_tiny_ttf_create_data_ex(chat_sc_start, chat_sc_end - chat_sc_start,
                                                 px, LV_FONT_KERNING_NONE, 12);
    return font ? font : fallback;
}

enum { DIAL_SIZE = 324, DIAL_RADIUS = 154 };

static lv_point_precise_t dial_point(int x, int y, int angle, int radius)
{
    return (lv_point_precise_t){
        x + radius * lv_trigo_sin(angle) / LV_TRIGO_SIN_MAX,
        y - radius * lv_trigo_cos(angle) / LV_TRIGO_SIN_MAX,
    };
}

static void draw_dial(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    int x = area.x1 + DIAL_SIZE / 2, y = area.y1 + DIAL_SIZE / 2;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t pen;
    lv_draw_line_dsc_init(&pen);
    pen.color = lv_color_black();
    pen.width = HOME_RULE_WIDTH;
    for (int minute = 0; minute < 60; ++minute) {
        int length = minute % 5 == 0 ? 15 : 5;
        pen.p1 = dial_point(x, y, minute * 6, DIAL_RADIUS - length);
        pen.p2 = dial_point(x, y, minute * 6, DIAL_RADIUS);
        lv_draw_line(layer, &pen);
    }
    if (dial_minute < 0) return;

    int hour_angle = (dial_minute % 720) / 2;
    int minute_angle = (dial_minute % 60) * 6;
    pen.width = 7;
    pen.p1 = (lv_point_precise_t){x, y};
    pen.p2 = dial_point(x, y, hour_angle, 69);
    lv_draw_line(layer, &pen);
    pen.width = 3;
    pen.p2 = dial_point(x, y, minute_angle, 99);
    lv_draw_line(layer, &pen);
    lv_draw_rect_dsc_t hub;
    lv_draw_rect_dsc_init(&hub);
    hub.bg_color = lv_color_black();
    hub.radius = LV_RADIUS_CIRCLE;
    lv_area_t hub_area = {x - 5, y - 5, x + 5, y + 5};
    lv_draw_rect(layer, &hub, &hub_area);
}

static void make_standby(lv_obj_t *parent)
{
    standby = box(parent);
    lv_obj_set_flex_grow(standby, 1);
    lv_obj_set_flex_flow(standby, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(standby, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(standby, 36, 0);

    dial = box(standby);
    lv_obj_set_size(dial, DIAL_SIZE, DIAL_SIZE);
    lv_obj_add_event_cb(dial, draw_dial, LV_EVENT_DRAW_MAIN, NULL);
    static const struct { const char *label; int x, y; } quarters[] = {
        {"12", 0, -115}, {"3", 115, 0}, {"6", 0, 115}, {"9", -115, 0},
    };
    for (unsigned i = 0; i < sizeof(quarters) / sizeof(quarters[0]); ++i) {
        lv_obj_t *num = eink_label(dial, quarters[i].label, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
        lv_obj_align(num, LV_ALIGN_CENTER, quarters[i].x, quarters[i].y);
    }
    sync_message = box(dial);
    lv_obj_set_size(sync_message, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sync_message, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sync_message, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sync_message, 7, 0);
    eink_label(sync_message, "等待校时", eink_heading_font(), LV_TEXT_ALIGN_CENTER);
    eink_label(sync_message, "联网后自动同步", &home_controls, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(sync_message);

    date_row = box(standby);
    lv_obj_set_size(date_row, LV_PCT(100), 86);
    date_label = eink_label(date_row, "", &lv_font_montserrat_28, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(date_label, 1, 0);
    lv_obj_align(date_label, LV_ALIGN_LEFT_MID, 0, -15);
    weekday_label = eink_label(date_row, "", &home_controls, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(weekday_label, LV_ALIGN_LEFT_MID, 0, 16);
    clock_label = eink_label(date_row, "", title_font, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void refresh_clock(bool idle)
{
    static const char *const weekdays[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    time_t now = time(NULL);
    struct tm date;
    bool valid = localtime_r(&now, &date) && date.tm_year >= 120 && date.tm_year <= 199;
    char hm[8] = "";
    if (valid) strftime(hm, sizeof(hm), "%H:%M", &date);
    if (!idle) {
        set_text(event_time, hm);
        return;
    }
    int minute = valid ? date.tm_hour * 60 + date.tm_min : -1;
    if (dial_minute != minute) {
        dial_minute = minute;
        lv_obj_invalidate(dial);
    }
    show(date_row, valid);
    show(sync_message, !valid);
    if (valid) {
        char ymd[20];
        strftime(ymd, sizeof(ymd), "%Y.%m.%d", &date);
        set_text(date_label, ymd);
        set_text(weekday_label, weekdays[date.tm_wday]);
        set_text(clock_label, hm);
    }
}

static lv_obj_t *response_link(lv_obj_t *parent, bool secondary)
{
    lv_obj_t *button = lv_button_create(parent);
    eink_style_clean_box(button);
    lv_obj_set_size(button, LV_PCT(100), secondary ? 48 : 62);
    home_edge(button, LV_BORDER_SIDE_TOP);
    lv_obj_set_style_bg_color(button, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_black(), 0);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &home_controls, 0);
    lv_label_set_text(label, "");
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *arrow = home_glyph(button, HOME_ARROW, secondary ? 22 : 30);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(button, open_response, LV_EVENT_CLICKED, secondary ? (void *)1 : NULL);
    return button;
}

static void make_event(lv_obj_t *parent)
{
    event_view = box(parent);
    lv_obj_set_flex_grow(event_view, 1);
    lv_obj_set_flex_flow(event_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *meta = box(event_view);
    lv_obj_set_height(meta, 42);
    category = eink_label(meta, "", eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_align(category, LV_ALIGN_LEFT_MID, 0, 0);
    event_time = eink_label(meta, "", &lv_font_montserrat_20, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(event_time, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *body = box(event_view);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, 16, 0);
    const home_glyph_t shapes[APP_COUNT] = {HOME_WAVE, HOME_PLAY, HOME_CARD, HOME_TIMER, HOME_BOOK};
    for (int i = 0; i < APP_COUNT; ++i) {
        glyphs[i] = home_glyph(body, shapes[i], 72);
        lv_obj_add_flag(glyphs[i], LV_OBJ_FLAG_HIDDEN);
    }
    headline = line_label(body, title_font, 134);
    detail = line_label(body, eink_body_font(), 122);
    lv_obj_set_style_text_line_space(detail, 5, 0);

    progress = lv_bar_create(body);
    lv_obj_remove_style_all(progress);
    lv_obj_set_size(progress, LV_PCT(100), 8);
    home_edge(progress, LV_BORDER_SIDE_FULL);
    lv_obj_set_style_bg_color(progress, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_INDICATOR);
    open_button = response_link(event_view, false);
}

static void countdown(char *text, size_t size, int seconds)
{
    if (seconds >= 3600)
        snprintf(text, size, "%02d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else snprintf(text, size, "%02d:%02d", seconds / 60, seconds % 60);
}

void home_response_refresh(void)
{
    desk_state_t desk;
    device_apps_snapshot(&desk);
    bool unread = desk.card.id[0] && !desk.card.acknowledged;
    bool timing = desk.seconds_left > 0;
    const desk_notice_t *n = &desk.notice;
    const char *section = "", *title = "", *body = "", *link = "";
    const lv_font_t *font = title_font;
    char digits[32], state[96];
    target = APP_COUNT;
    book_id[0] = 0;
    bool countdown_visible = false;
    if (n->kind != DESK_NONE) {
        target = n->app;
        section = app_titles[target];
        switch (n->kind) {
        case DESK_BOOK: case DESK_CHAPTER:
            section = n->kind == DESK_BOOK ? "阅读书库 / 已收到书籍" : "阅读书库 / 已收到章节";
            title = n->title;
            link = "打开这本书";
            strlcpy(book_id, n->book_id, sizeof(book_id));
            break;
        case DESK_PULSE: title = "震动提醒"; body = n->title; break;
        case DESK_PLAYBACK:
            title = n->title;
            player_description(state, sizeof(state));
            body = state;
            link = "查看播放器";
            break;
        case DESK_STOPPED: title = "已收到停止请求"; break;
        case DESK_ERROR: title = "调用未完成"; body = n->title; break;
        default: break;
        }
    } else if (desk.show_timer_end) {
        target = APP_TIMER;
        section = "计时器"; title = "时间到了"; body = desk.timer_label; link = "查看计时器";
    } else if (unread && (!timing || desk.focus == APP_CARD)) {
        target = APP_CARD;
        section = "提示卡片"; title = desk.card.title; body = desk.card.body; link = "查看并确认";
    } else if (timing) {
        target = APP_TIMER;
        section = "计时器"; countdown(digits, sizeof(digits), desk.seconds_left);
        title = digits;
        font = desk.seconds_left >= 3600 ? hour_font : clock_font;
        countdown_visible = true;
        body = desk.timer_label; link = "打开计时器";
    }
    bool idle = target == APP_COUNT;
    show(standby, idle);
    show(event_view, !idle);
    refresh_clock(idle);
    if (!idle) {
        for (int i = 0; i < APP_COUNT; ++i) show(glyphs[i], (device_app_t)i == target);
        set_text(category, section);
        if (lv_obj_get_style_text_font(headline, 0) != font) {
            lv_obj_set_style_text_font(headline, font, 0);
            lv_obj_set_style_max_height(headline, countdown_visible ? lv_font_get_line_height(font) : 134, 0);
        }
        if (strcmp(last_title, title)) {
            strlcpy(last_title, title, sizeof(last_title));
            lv_label_set_text(headline, last_title);
        }
        if (strcmp(last_body, body)) {
            strlcpy(last_body, body, sizeof(last_body));
            lv_label_set_text(detail, last_body);
        }
        show(detail, body[0] != 0);
        show(progress, countdown_visible);
        if (countdown_visible && desk.timer_seconds > 0) {
            int percent = (desk.timer_seconds - desk.seconds_left) * 100 / desk.timer_seconds;
            if (lv_bar_get_value(progress) != percent) lv_bar_set_value(progress, percent, LV_ANIM_OFF);
        }
        set_text(lv_obj_get_child(open_button, 0), link);
        show(open_button, link[0] != 0);
    }
    context_target = APP_COUNT;
    if (timing && target != APP_TIMER) {
        countdown(digits, sizeof(digits), desk.seconds_left);
        snprintf(state, sizeof(state), "倒计时  %s", digits);
        context_target = APP_TIMER;
    } else if (unread && target != APP_CARD) {
        strlcpy(state, "查看未读卡片", sizeof(state));
        context_target = APP_CARD;
    }
    if (context_target != APP_COUNT) set_text(context_label, state);
    show(context, context_target != APP_COUNT);
    if (desk.query_feedback) set_text(query_label, desk.query_feedback);
    show(query_label, desk.query_feedback != NULL);
}

void home_response_create(lv_obj_t *parent)
{
    home = lv_obj_get_screen(parent);
    if (!clock_font) clock_font = display_font(126, &lv_font_montserrat_48);
    if (!hour_font) hour_font = display_font(78, &lv_font_montserrat_48);
    if (!title_font) title_font = display_font(48, eink_heading_font());
    last_title[0] = last_body[0] = 0;
    lv_obj_t *area = box(parent);
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(area, HOME_MARGIN, 0);
    lv_obj_set_style_pad_top(area, 18, 0);
    lv_obj_set_style_pad_bottom(area, 26, 0);
    make_standby(area);
    make_event(area);
    context = response_link(area, true);
    context_label = lv_obj_get_child(context, 0);
    query_label = line_label(area, eink_small_font(), 27);
    home_response_refresh();
}
