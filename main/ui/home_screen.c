#include "home_screen.h"
#include "home_response.h"
#include "home_graphics.h"
#include "device_apps.h"
#include "status_bar.h"
#include "eink_ui.h"
#include "reader_screen.h"
#include "schedule_screen.h"
#include "settings_screen.h"

#include "lvgl.h"

static lv_obj_t *home_scr;
static lv_obj_t *upload_origin;
static lv_obj_t *upload_panel;
static lv_obj_t *duration_buttons[4];
static lv_timer_t *desk_timer;
static recorder_state_t recording_state;
static const uint32_t durations[] = {15, 30, 60, 120};
static unsigned selected_duration = 1;
static int battery_level = -1;

static void set_duration(unsigned selection)
{
    selected_duration = selection;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == selected_duration) {
            lv_obj_add_state(duration_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(duration_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void select_duration(lv_event_t *event)
{
    set_duration((unsigned)(uintptr_t)lv_event_get_user_data(event));
}

static void cancel_upload(lv_event_t *event)
{
    (void)event;
    home_screen_show_upload_confirmation(false);
}

static lv_obj_t *panel(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    eink_style_clean_box(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(obj, LV_PCT(100));
    return obj;
}

static void add_rule(lv_obj_t *parent, lv_align_t anchor, int y)
{
    lv_obj_t *rule = eink_divider(parent, LV_PCT(100));
    lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_height(rule, HOME_RULE_WIDTH);
    lv_obj_align(rule, anchor, 0, y);
}

static void open_schedule(lv_event_t *event)
{
    (void)event;
    schedule_screen_open(home_scr);
}

static void open_shelf(lv_event_t *event)
{
    (void)event;
    reader_open(home_scr, NULL);
}

static void refresh_desk(lv_timer_t *timer)
{
    (void)timer;
    device_apps_tick();
    status_bar_update(recording_state, battery_level);
    if (lv_screen_active() == home_scr && !home_screen_upload_confirmation_visible())
        home_response_refresh();
}

static void open_settings(lv_event_t *event)
{
    (void)event;
    settings_screen_open(home_scr);
}

static void make_dock(lv_obj_t *parent)
{
    static const struct { const char *label; home_glyph_t glyph; lv_event_cb_t open; } entries[] = {
        {"日程", HOME_AGENDA, open_schedule},
        {"书架", HOME_BOOK, open_shelf}, {"设置", HOME_SETTINGS, open_settings},
    };
    lv_obj_t *dock = panel(parent);
    lv_obj_set_height(dock, 112);
    lv_obj_set_style_pad_hor(dock, 24, 0);
    lv_obj_t *rule = eink_divider(dock, LV_PCT(100));
    lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_height(rule, HOME_RULE_WIDTH);
    lv_obj_set_width(rule, lv_display_get_horizontal_resolution(NULL) - 56);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        lv_obj_t *btn = lv_button_create(dock);
        eink_style_clean_box(btn);
        lv_obj_set_size(btn, 0, LV_PCT(100));
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_text_color(btn, lv_color_black(), 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, lv_color_black(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_pad_row(btn, 6, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        home_glyph(btn, entries[i].glyph, 30);
        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, eink_body_font(), 0);
        lv_label_set_text(label, entries[i].label);
        lv_obj_add_event_cb(btn, entries[i].open, LV_EVENT_CLICKED, NULL);
    }
}

static lv_obj_t *make_duration_chip(lv_obj_t *parent, int x, int y, int width, const char *text)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, width, 64);
    home_edge(btn, LV_BORDER_SIDE_FULL);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(btn, &home_controls, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

void home_screen_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    home_scr = screen;
    lv_display_t *display = lv_display_get_default();
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);
    const int inner_w = width - HOME_MARGIN * 2;

    lv_obj_clean(screen);
    eink_style_screen(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(screen, width, height);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    status_bar_create(screen);
    lv_obj_t *home_content = panel(screen);
    lv_obj_set_flex_grow(home_content, 1);
    lv_obj_set_flex_flow(home_content, LV_FLEX_FLOW_COLUMN);
    home_response_create(home_content);
    if (!desk_timer) desk_timer = lv_timer_create(refresh_desk, 250, NULL);
    make_dock(home_content);

    // A short BOOT press during recording opens this panel.
    upload_panel = lv_obj_create(NULL);
    eink_style_screen(upload_panel);
    lv_obj_remove_flag(upload_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(upload_panel, width, height);
    lv_obj_set_style_pad_all(upload_panel, HOME_MARGIN, LV_PART_MAIN);

    lv_obj_t *upload_card = lv_obj_create(upload_panel);
    eink_style_clean_box(upload_card);
    lv_obj_remove_flag(upload_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(upload_card, LV_PCT(100), LV_PCT(100));

    lv_obj_t *up_title = eink_label(upload_card, "上传当前录音", eink_heading_font(),
                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(up_title, 0, 38);
    lv_obj_set_width(up_title, inner_w);

    lv_obj_t *up_sub = eink_label(upload_card, "从本次录音起始位置截取时长", eink_small_font(),
                                  LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(up_sub, 0, 96);
    add_rule(upload_card, LV_ALIGN_TOP_MID, 148);
    lv_obj_set_width(up_sub, inner_w);

    const char *labels[] = {"15 分钟", "30 分钟", "1 小时", "2 小时"};
    const int chip_w = (inner_w - 14) / 2;
    for (unsigned i = 0; i < 4; ++i) {
        duration_buttons[i] = make_duration_chip(
            upload_card, (i % 2) * (chip_w + 14), 182 + (i / 2) * 78, chip_w, labels[i]);
        lv_obj_add_event_cb(duration_buttons[i], select_duration, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
    lv_obj_add_state(duration_buttons[selected_duration], LV_STATE_CHECKED);

    lv_obj_t *up_hint = eink_label(upload_card,
                                   "再次短按 BOOT 确认上传\n若录音不足所选时长则全部上传",
                                   eink_small_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(up_hint, 0, 370);
    lv_obj_set_width(up_hint, inner_w);
    lv_obj_set_style_text_line_space(up_hint, 8, LV_PART_MAIN);

    lv_obj_t *cancel = eink_pill_button(upload_card, "取消", &home_controls, 56, false,
                                        cancel_upload, NULL);
    lv_obj_set_width(cancel, inner_w);
    home_edge(cancel, LV_BORDER_SIDE_FULL);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void home_screen_show_upload_confirmation(bool visible)
{
    if (!upload_panel || visible == home_screen_upload_confirmation_visible()) return;
    if (visible) {
        upload_origin = lv_screen_active();
        settings_screen_hide();
        set_duration(1); // Default to the first 30 minutes of the current recording.
        lv_screen_load(upload_panel);
    } else {
        lv_screen_load(upload_origin ? upload_origin : home_scr);
        upload_origin = NULL;
    }
}

bool home_screen_upload_confirmation_visible(void)
{
    return upload_panel && lv_screen_active() == upload_panel;
}

uint32_t home_screen_upload_minutes(void)
{
    return durations[selected_duration];
}

void home_screen_set_status(recorder_state_t recorder, int battery_percent)
{
    const bool recorder_changed = recording_state != recorder;
    recording_state = recorder;
    if (recorder_changed || battery_level != battery_percent) {
        battery_level = battery_percent;
        status_bar_update(recording_state, battery_level);
    }
    if (recorder_changed) {
        home_response_set_recorder(recording_state);
        settings_screen_refresh_state();
    }
}
