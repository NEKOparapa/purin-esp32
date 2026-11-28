#include "home_response.h"
#include "device_apps_screen.h"
#include "eink_ui.h"
#include "home_graphics.h"
#include <stdio.h>
#include <string.h>

extern const uint8_t ui_sc_start[] asm("_binary_ui_sc_ttf_start");
extern const uint8_t ui_sc_end[] asm("_binary_ui_sc_ttf_end");
static lv_obj_t *home, *standby, *cat, *record_hint, *record_stop_hint;
static lv_obj_t *event_view, *event_body, *category, *headline, *detail, *open_button, *action_arrow;
static lv_obj_t *progress, *total_time, *glyphs[APP_COUNT];
static const lv_font_t *clock_font, *hour_font, *title_font;
static desk_notice_kind_t displayed_kind;
static char card_id[sizeof(((prompt_card_t *)0)->id)];
static uint32_t displayed_timer_run;
static bool pressed_current, acknowledge_failed;
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

static void response_action(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        pressed_current = true;
        return;
    }
    if (!pressed_current) return;
    pressed_current = false;
    switch (displayed_kind) {
    case DESK_CARD:
        acknowledge_failed = !device_apps_acknowledge(card_id);
        home_response_refresh();
        break;
    case DESK_TIMER:
        device_apps_stop_home_timer(displayed_timer_run);
        home_response_refresh();
        break;
    case DESK_BOOK: case DESK_CHAPTER:
        if (book_id[0]) device_book_open(book_id, home);
        break;
    case DESK_PLAYBACK:
        device_app_open(APP_PLAYER, home);
        break;
    default: break;
    }
}

static const lv_font_t *display_font(int px, const lv_font_t *fallback)
{
    lv_font_t *font = lv_tiny_ttf_create_data_ex(ui_sc_start, ui_sc_end - ui_sc_start,
                                                 px, LV_FONT_KERNING_NONE, 12);
    return font ? font : fallback;
}

extern const uint8_t home_cat_pixels[] asm("_binary_home_cat_i1_start");
static const lv_image_dsc_t home_cat = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I1,
               .w = 208, .h = 202, .stride = 26},
    .data_size = 8 + 26 * 202, .data = home_cat_pixels,
};

static void make_standby(lv_obj_t *parent)
{
    standby = box(parent);
    lv_obj_set_flex_grow(standby, 1);

    cat = lv_image_create(standby);
    lv_image_set_src(cat, &home_cat);

    lv_obj_t *hints = box(standby);
    lv_obj_set_height(hints, 85);
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(hints, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hints, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hints, 4, 0);

    lv_obj_t *first_line = box(hints);
    lv_obj_set_size(first_line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(first_line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(first_line, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(first_line, 8, 0);
    home_glyph(first_line, HOME_KEY, 24);
    record_hint = eink_label(first_line, "", eink_small_font(), LV_TEXT_ALIGN_CENTER);
    record_stop_hint = eink_label(hints, "长按 2 秒停止录音", eink_small_font(), LV_TEXT_ALIGN_CENTER);
    home_response_set_recorder(RECORDER_IDLE);
}

void home_response_set_recorder(recorder_state_t state)
{
    if (!record_hint) return;
    static const char *const hints[] = {
        [RECORDER_IDLE] = "",
        [RECORDER_STARTING] = "",
        [RECORDER_RECORDING] = "短按 BOOT 键选择上传片段",
        [RECORDER_STOPPING] = "",
        [RECORDER_ERROR] = "录音异常",
    };
    lv_obj_align(cat, LV_ALIGN_CENTER, 0, hints[state][0] ? -68 : 0);
    set_text(record_hint, hints[state]);
    show(lv_obj_get_parent(record_hint), hints[state][0] != 0);
    show(record_stop_hint, state == RECORDER_RECORDING);
}

static lv_obj_t *response_link(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    eink_style_clean_box(button);
    lv_obj_set_size(button, LV_PCT(100), 62);
    home_edge(button, LV_BORDER_SIDE_TOP);
    lv_obj_set_style_bg_color(button, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_black(), 0);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, eink_body_font(), 0);
    lv_label_set_text(label, "");
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    action_arrow = home_glyph(button, HOME_ARROW, 30);
    lv_obj_align(action_arrow, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(button, response_action, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(button, response_action, LV_EVENT_CLICKED, NULL);
    return button;
}

static void make_event(lv_obj_t *parent)
{
    event_view = box(parent);
    lv_obj_set_flex_grow(event_view, 1);
    lv_obj_set_flex_flow(event_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(event_view, 18, 0);
    lv_obj_set_style_pad_bottom(event_view, 26, 0);
    lv_obj_t *meta = box(event_view);
    lv_obj_set_height(meta, 42);
    category = eink_label(meta, "", eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_align(category, LV_ALIGN_LEFT_MID, 0, 0);
    event_body = box(event_view);
    lv_obj_set_scroll_dir(event_body, LV_DIR_VER);
    lv_obj_set_flex_grow(event_body, 1);
    lv_obj_set_flex_flow(event_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(event_body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(event_body, 16, 0);
    const home_glyph_t shapes[APP_COUNT] = {HOME_WAVE, HOME_PLAY, HOME_CARD, HOME_TIMER, HOME_BOOK};
    for (int i = 0; i < APP_COUNT; ++i) {
        glyphs[i] = home_glyph(event_body, shapes[i], 72);
        lv_obj_add_flag(glyphs[i], LV_OBJ_FLAG_HIDDEN);
    }
    headline = line_label(event_body, title_font, 134);
    detail = line_label(event_body, eink_body_font(), 122);
    lv_obj_set_style_text_line_space(detail, 5, 0);

    progress = lv_bar_create(event_body);
    lv_obj_remove_style_all(progress);
    lv_obj_set_size(progress, LV_PCT(100), 8);
    home_edge(progress, LV_BORDER_SIDE_FULL);
    lv_obj_set_style_bg_color(progress, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_INDICATOR);
    total_time = eink_label(event_body, "", eink_small_font(), LV_TEXT_ALIGN_LEFT);
    open_button = response_link(event_view);
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
    const desk_notice_t *n = &desk.notice;
    const char *section = "", *title = "", *body = "", *link = "";
    const lv_font_t *font = title_font;
    char digits[32], state[96];
    bool new_event = displayed_kind != n->kind ||
                     (n->kind == DESK_CARD && strcmp(card_id, desk.card.id)) ||
                     (n->kind == DESK_TIMER && displayed_timer_run != desk.timer_run) ||
                     strcmp(book_id, n->book_id);
    const bool inline_view = n->kind == DESK_CARD || n->kind == DESK_TIMER || n->kind == DESK_TIMER_END;
    const bool countdown_visible = n->kind == DESK_TIMER;
    if (new_event) {
        displayed_kind = n->kind;
        displayed_timer_run = desk.timer_run;
        strlcpy(card_id, desk.card.id, sizeof(card_id));
        strlcpy(book_id, n->book_id, sizeof(book_id));
        pressed_current = acknowledge_failed = false;
        lv_obj_scroll_to_y(event_body, 0, LV_ANIM_OFF);
        lv_obj_set_flag(event_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE, inline_view);
        lv_obj_set_flex_align(event_body, inline_view ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_label_long_mode_t mode = inline_view ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS;
        lv_label_set_long_mode(headline, mode);
        lv_label_set_long_mode(detail, mode);
        lv_obj_set_style_max_height(headline, inline_view ? LV_COORD_MAX : 134, 0);
        lv_obj_set_style_max_height(detail, inline_view ? LV_COORD_MAX : 122, 0);
    }
    if (n->kind != DESK_NONE) {
        section = app_titles[n->app];
    }
    switch (n->kind) {
    case DESK_CARD:
        title = desk.card.title;
        body = desk.card.body;
        font = eink_heading_font();
        link = acknowledge_failed ? "确认失败，点此重试" : "我看过了";
        break;
    case DESK_TIMER:
        countdown(digits, sizeof(digits), desk.seconds_left);
        title = digits;
        font = desk.seconds_left >= 3600 ? hour_font : clock_font;
        body = desk.timer_label;
        link = "结束计时";
        break;
    case DESK_TIMER_END:
        title = "时间到了";
        body = desk.timer_label;
        break;
    case DESK_BOOK: case DESK_CHAPTER:
        section = n->kind == DESK_BOOK ? "书库管理 / 已收到书籍" : "书库管理 / 已收到章节";
        title = n->title;
        link = "打开这本书";
        break;
    case DESK_BOOK_DELETED: title = "已从书库删除"; break;
    case DESK_PULSE: title = "震动提醒"; body = n->title; break;
    case DESK_PLAYBACK:
        title = n->title;
        player_description(state, sizeof(state));
        body = state;
        link = "查看播放器";
        break;
    case DESK_STOPPED: title = "已收到停止请求"; break;
    case DESK_ERROR: title = "调用未完成"; body = n->title; break;
    case DESK_NONE: break;
    }
    bool idle = n->kind == DESK_NONE;
    show(standby, idle);
    show(event_view, !idle);
    if (!idle) {
        for (int i = 0; i < APP_COUNT; ++i)
            show(glyphs[i], n->kind != DESK_CARD && (device_app_t)i == n->app);
        set_text(category, section);
        if (lv_obj_get_style_text_font(headline, 0) != font) {
            lv_obj_set_style_text_font(headline, font, 0);
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
        show(total_time, countdown_visible);
        if (countdown_visible && desk.timer_seconds > 0) {
            char duration[32], caption[64];
            countdown(duration, sizeof(duration), desk.timer_seconds);
            snprintf(caption, sizeof(caption), "总时长 %s", duration);
            set_text(total_time, caption);
            int percent = (desk.timer_seconds - desk.seconds_left) * 100 / desk.timer_seconds;
            if (lv_bar_get_value(progress) != percent) lv_bar_set_value(progress, percent, LV_ANIM_OFF);
        }
        set_text(lv_obj_get_child(open_button, 0), link);
        show(action_arrow, !inline_view);
        show(open_button, link[0] != 0);
    }
}

void home_response_create(lv_obj_t *parent)
{
    home = lv_obj_get_screen(parent);
    if (!clock_font) clock_font = display_font(126, &lv_font_montserrat_48);
    if (!hour_font) hour_font = display_font(78, &lv_font_montserrat_48);
    if (!title_font) title_font = display_font(48, eink_heading_font());
    last_title[0] = last_body[0] = 0;
    displayed_kind = DESK_NONE;
    book_id[0] = 0;
    lv_obj_t *area = box(parent);
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(area, HOME_MARGIN, 0);
    make_standby(area);
    make_event(area);
    home_response_refresh();
}
