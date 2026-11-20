#include "home_screen.h"
#include "home_response.h"
#include "home_graphics.h"
#include "device_apps.h"
#include <stdio.h>
#include "agent_screen.h"
#include "eink_ui.h"
#include "reader_screen.h"
#include "settings_screen.h"

#include <string.h>
#include "lvgl.h"

static lv_obj_t *home_scr;
static lv_obj_t *recording_icon;
static lv_obj_t *recording_wave;
static lv_obj_t *upload_icon;
static lv_obj_t *status_details;
static lv_obj_t *home_content;
static lv_obj_t *upload_panel;
static lv_obj_t *duration_buttons[4];
static lv_obj_t *schedule_scr;
static lv_timer_t *desk_timer;
static recorder_state_t recording_state;
static recorder_error_t recording_error;
static audio_upload_state_t upload_state;
static const uint32_t durations[] = {30, 15, 60, 120};
static unsigned selected_duration;

static void sync_status_icons(void)
{
    static const char *const recording_icons[] = {
        [RECORDER_IDLE] = "",
        [RECORDER_STARTING] = LV_SYMBOL_REFRESH,
        [RECORDER_RECORDING] = "",
        [RECORDER_STOPPING] = LV_SYMBOL_SAVE,
        [RECORDER_ERROR] = LV_SYMBOL_WARNING,
    };
    static const char *const upload_icons[] = {
        [AUDIO_UPLOAD_IDLE] = "",
        [AUDIO_UPLOAD_PREPARING] = LV_SYMBOL_FILE,
        [AUDIO_UPLOAD_CONNECTING] = LV_SYMBOL_LOOP,
        [AUDIO_UPLOAD_SENDING] = LV_SYMBOL_UPLOAD,
        [AUDIO_UPLOAD_ACCEPTED] = LV_SYMBOL_UPLOAD " " LV_SYMBOL_OK,
        [AUDIO_UPLOAD_FAILED] = LV_SYMBOL_CLOSE,
        [AUDIO_UPLOAD_NOT_CONFIGURED] = LV_SYMBOL_SETTINGS,
    };

    lv_label_set_text_static(recording_icon, recording_icons[recording_state]);
    lv_label_set_text_static(upload_icon, upload_icons[upload_state]);
    lv_obj_set_flag(recording_icon, LV_OBJ_FLAG_HIDDEN,
                    recording_state == RECORDER_IDLE || recording_state == RECORDER_RECORDING);
    lv_obj_set_flag(recording_wave, LV_OBJ_FLAG_HIDDEN, recording_state != RECORDER_RECORDING);
    lv_obj_set_flag(upload_icon, LV_OBJ_FLAG_HIDDEN, upload_state == AUDIO_UPLOAD_IDLE);
    lv_obj_set_flag(lv_obj_get_parent(recording_icon), LV_OBJ_FLAG_HIDDEN,
                    recording_state == RECORDER_IDLE && upload_state == AUDIO_UPLOAD_IDLE);
}

static const char *status_label_zh(const char *text)
{
    static const struct { const char *raw, *label; } labels[] = {
        {"Ready", "就绪"}, {"Starting recording", "准备录音"}, {"Recording", "录音中"}, {"Saving", "保存录音"},
        {"Preparing audio", "准备音频"}, {"Connecting network", "正在联网"}, {"Uploading audio", "上传中"},
        {"Audio accepted for processing", "音频已接收"}, {"Upload failed; tap BOOT to retry", "上传失败，按键重试"},
        {"Upload failed; audio kept on SD", "上传失败，录音已保留"}, {"Configure network and upload server", "待配置上传"},
        {"Enable network in Settings", "请在设置中开启网络"},
        {"Not enough memory", "内存不足"}, {"SD card unavailable", "存储卡不可用"}, {"SD write failed", "存储卡写入失败"},
        {"SD card full", "存储卡已满"}, {"Microphone error", "麦克风异常"}, {"No audio input", "无音频输入"},
        {"Audio buffer overrun", "录音缓冲区已满"}, {"Audio encoding failed", "音频编码失败"}, {"Recording error", "录音异常"},
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        if (strcmp(text, labels[i].raw) == 0) return labels[i].label;
    }
    return text;
}

static void refresh_status_details(void)
{
    const char *rec = recorder_error_text(recording_error);
    switch (recording_state) {
    case RECORDER_STARTING: rec = "Starting recording"; break;
    case RECORDER_RECORDING: rec = "Recording"; break;
    case RECORDER_STOPPING: rec = "Saving"; break;
    default: break;
    }
    const char *upload = upload_state == AUDIO_UPLOAD_FAILED && recording_state != RECORDER_RECORDING
        ? "Upload failed; audio kept on SD" : audio_upload_status_text(upload_state);
    lv_obj_t *label = lv_obj_get_child(status_details, 0);
    if (upload[0] && recording_state != RECORDER_IDLE) {
        lv_label_set_text_fmt(label, "%s\n%s", status_label_zh(rec), status_label_zh(upload));
    } else {
        lv_label_set_text_static(label, status_label_zh(upload[0] ? upload : rec));
    }
}

static void hide_status_details(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(status_details, LV_OBJ_FLAG_HIDDEN);
}

static void toggle_status_details(lv_event_t *event)
{
    if (lv_obj_has_flag(status_details, LV_OBJ_FLAG_HIDDEN)) {
        refresh_status_details();
        lv_obj_remove_flag(status_details, LV_OBJ_FLAG_HIDDEN);
    } else {
        hide_status_details(event);
    }
}

static void return_home(lv_event_t *event)
{
    (void)event;
    if (home_scr) lv_screen_load(home_scr);
}

static void select_duration(lv_event_t *event)
{
    selected_duration = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    for (unsigned i = 0; i < 4; ++i) {
        if (i == selected_duration) {
            lv_obj_add_state(duration_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(duration_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void cancel_upload(lv_event_t *event)
{
    (void)event;
    home_screen_show_upload_confirmation(false);
}

static void open_agent(lv_event_t *event)
{
    (void)event;
    agent_screen_open();
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
    if (!schedule_scr) {
        schedule_scr = lv_obj_create(NULL);
        eink_style_screen(schedule_scr);
        lv_obj_set_flex_flow(schedule_scr, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *header = panel(schedule_scr);
        lv_obj_set_height(header, 76);
        lv_obj_set_style_pad_hor(header, HOME_MARGIN, 0);
        lv_obj_t *back = eink_back_button(header, return_home);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, -12, 0);
        lv_obj_t *title = eink_label(header, "日程", eink_heading_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 48, 0);
        add_rule(header, LV_ALIGN_BOTTOM_MID, 0);

        lv_obj_t *body = panel(schedule_scr);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_bottom(body, 76, 0);
        lv_obj_set_style_pad_row(body, 18, 0);
        home_glyph(body, HOME_AGENDA, 62);
        eink_label(body, "暂无待办日程", eink_heading_font(), LV_TEXT_ALIGN_CENTER);
        eink_label(body, "提醒卡片会显示在主页", &home_controls, LV_TEXT_ALIGN_CENTER);
    }
    lv_screen_load(schedule_scr);
}

static void open_reading(lv_event_t *event)
{
    (void)event;
    reader_open(home_scr, NULL);
}

static void refresh_desk(lv_timer_t *timer)
{
    (void)timer;
    device_apps_tick();
    if (lv_screen_active() == home_scr && !home_screen_upload_confirmation_visible())
        home_response_refresh();
}

static void open_settings(lv_event_t *event)
{
    (void)event;
    settings_screen_open(home_scr);
}

static void make_header(lv_obj_t *screen)
{
    lv_obj_t *header = panel(screen);
    lv_obj_set_height(header, 76);
    lv_obj_set_style_pad_hor(header, HOME_MARGIN, 0);
    lv_obj_t *mark = home_glyph(header, HOME_MARK, 34);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *wordmark = eink_label(header, "WINTERMUTE", &lv_font_montserrat_20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(wordmark, 2, 0);
    lv_obj_align(wordmark, LV_ALIGN_LEFT_MID, 44, 0);

    lv_obj_t *state = lv_button_create(header);
    eink_style_clean_box(state);
    lv_obj_set_size(state, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_min_width(state, 48, 0);
    lv_obj_set_style_pad_hor(state, 8, 0);
    lv_obj_align(state, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(state, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state, 12, 0);
    recording_icon = eink_label(state, "", &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER);
    recording_wave = home_glyph(state, HOME_WAVE, 28);
    upload_icon = eink_label(state, "", &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_event_cb(state, toggle_status_details, LV_EVENT_CLICKED, NULL);
    sync_status_icons();
    add_rule(header, LV_ALIGN_BOTTOM_MID, 0);
}

static void make_dock(lv_obj_t *parent)
{
    static const struct { const char *label; home_glyph_t glyph; lv_event_cb_t open; } entries[] = {
        {"对话", HOME_CHAT, open_agent}, {"日程", HOME_AGENDA, open_schedule},
        {"阅读", HOME_BOOK, open_reading}, {"设置", HOME_SETTINGS, open_settings},
    };
    lv_obj_t *dock = panel(parent);
    lv_obj_set_height(dock, 92);
    lv_obj_set_style_pad_hor(dock, HOME_MARGIN, 0);
    add_rule(dock, LV_ALIGN_TOP_MID, 0);
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
        lv_obj_set_style_text_font(label, &home_controls, 0);
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

    make_header(screen);
    lv_obj_t *workspace = panel(screen);
    lv_obj_set_flex_grow(workspace, 1);
    home_content = panel(workspace);
    lv_obj_set_height(home_content, LV_PCT(100));
    lv_obj_set_flex_flow(home_content, LV_FLEX_FLOW_COLUMN);
    home_response_create(home_content);
    if (!desk_timer) desk_timer = lv_timer_create(refresh_desk, 250, NULL);
    make_dock(home_content);

    // A short BOOT press during recording opens this panel.
    upload_panel = lv_obj_create(workspace);
    eink_style_clean_box(upload_panel);
    lv_obj_remove_flag(upload_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(upload_panel, LV_PCT(100), LV_PCT(100));
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

    const char *labels[] = {"30 分钟", "15 分钟", "1 小时", "2 小时"};
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
    lv_obj_add_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);

    status_details = lv_obj_create(screen);
    eink_style_clean_box(status_details);
    eink_style_card(status_details, 12, 2);
    lv_obj_remove_flag(status_details, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(status_details, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(status_details, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(status_details, 14, 0);
    lv_obj_align(status_details, LV_ALIGN_TOP_RIGHT, -HOME_MARGIN, 80);
    eink_label(status_details, "", eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_add_event_cb(status_details, hide_status_details, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(screen, hide_status_details, LV_EVENT_SCREEN_UNLOADED, NULL);
}

void home_screen_show_upload_confirmation(bool visible)
{
    if (!upload_panel || !home_content) {
        return;
    }
    if (visible) {
        hide_status_details(NULL);
        agent_screen_hide();
        settings_screen_hide();
        if (home_scr && lv_screen_active() != home_scr) {
            lv_screen_load(home_scr);
        }
        lv_obj_remove_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(home_content, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(home_content, LV_OBJ_FLAG_HIDDEN);
    }
}

bool home_screen_upload_confirmation_visible(void)
{
    return upload_panel && !lv_obj_has_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);
}

uint32_t home_screen_upload_minutes(void)
{
    return durations[selected_duration];
}

void home_screen_set_status(recorder_status_t recorder, audio_upload_state_t upload)
{
    const bool icons_changed = recording_state != recorder.state || upload_state != upload;
    const bool details_changed = icons_changed || recording_error != recorder.error;
    recording_state = recorder.state;
    recording_error = recorder.error;
    upload_state = upload;
    if (icons_changed && recording_icon) sync_status_icons();
    if (details_changed && status_details && !lv_obj_has_flag(status_details, LV_OBJ_FLAG_HIDDEN)) {
        if (recording_state == RECORDER_IDLE && upload_state == AUDIO_UPLOAD_IDLE) {
            hide_status_details(NULL);
        } else {
            refresh_status_details();
        }
    }
    settings_screen_refresh_state();
}
