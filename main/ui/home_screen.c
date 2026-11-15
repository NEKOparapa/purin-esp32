#include "home_screen.h"
#include "agent_screen.h"
#include "notice_screen.h"

#include <string.h>
#include "lvgl.h"

static lv_obj_t *recording_icon;
static lv_obj_t *status_label;
static lv_obj_t *home_content;
static lv_obj_t *upload_panel;
static lv_obj_t *upload_status;
static lv_obj_t *duration_buttons[4];
static const uint32_t durations[] = {30, 15, 60, 120};
static unsigned selected_duration;

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

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int width,
                            const char *text, const lv_font_t *font,
                            lv_text_align_t alignment)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, alignment, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(label, text);
    return label;
}

static void make_divider(lv_obj_t *parent, int y, int width)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(line, 32, y);
    lv_obj_set_size(line, width, 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 64);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_color(button, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(button, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(button, lv_color_black(), 0);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_CHECKED);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void open_agent(lv_event_t *event)
{
    (void)event;
    agent_screen_open();
}

void home_screen_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    // 使用 LVGL 的逻辑分辨率布局，控制器原生寻址由显示适配层负责。
    lv_display_t *display = lv_display_get_default();
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);
    const int content_width = width - 64;
    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(screen, width, height);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    // 使用现有西文字体，避免占位页面引入字体资源和额外刷新逻辑。
    make_label(screen, 32, 40, content_width - 100, "WINTERMUTE", &lv_font_montserrat_28, LV_TEXT_ALIGN_LEFT);
    recording_icon = lv_obj_create(screen);
    lv_obj_remove_style_all(recording_icon);
    lv_obj_remove_flag(recording_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(recording_icon, width - 108, 40);
    lv_obj_set_size(recording_icon, 76, 36);
    lv_obj_t *dot = lv_obj_create(recording_icon);
    lv_obj_remove_style_all(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dot, 0, 10);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    make_label(recording_icon, 22, 4, 54, "REC", &lv_font_montserrat_20, LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(recording_icon, LV_OBJ_FLAG_HIDDEN);
    make_divider(screen, 112, content_width);
    home_content = lv_obj_create(screen);
    lv_obj_remove_style_all(home_content);
    lv_obj_remove_flag(home_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(home_content, 0, 130);
    lv_obj_set_size(home_content, width, height - 250);
    lv_obj_t *heading = make_label(home_content, 32, 150, content_width, "HOME", &lv_font_montserrat_48, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(heading, 72);
    make_label(home_content, 32, 230, content_width, "Hold BOOT: start / stop", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    make_label(home_content, 32, 270, content_width, "While recording, tap to upload", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    upload_status = make_label(home_content, 32, 350, content_width, "", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *chat = make_button(home_content, 32, 399, content_width, "Agent chat");
    lv_obj_add_event_cb(chat, open_agent, LV_EVENT_CLICKED, NULL);
    notice_screen_init(make_button(home_content, 32, 475, content_width, "Notifications"), heading);

    upload_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(upload_panel);
    lv_obj_remove_flag(upload_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(upload_panel, 32, 140);
    lv_obj_set_size(upload_panel, content_width, height - 250);
    make_label(upload_panel, 0, 10, content_width, "UPLOAD RECORDING", &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER);
    make_label(upload_panel, 0, 65, content_width, "From this recording's start", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    const char *labels[] = {"30 minutes", "15 minutes", "1 hour", "2 hours"};
    for (unsigned i = 0; i < 4; ++i) {
        duration_buttons[i] = make_button(upload_panel, (i % 2) * (content_width + 12) / 2,
                                          120 + (i / 2) * 88, (content_width - 12) / 2, labels[i]);
        lv_obj_add_event_cb(duration_buttons[i], select_duration, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_add_state(duration_buttons[0], LV_STATE_CHECKED);
    make_label(upload_panel, 0, 310, content_width, "Tap BOOT again to confirm", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    make_label(upload_panel, 0, 348, content_width, "Shorter recording: upload all", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *cancel = make_button(upload_panel, 0, 414, content_width, "Cancel");
    lv_obj_add_event_cb(cancel, cancel_upload, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);
    make_divider(screen, height - 96, content_width);
    status_label = make_label(screen, 32, height - 64, content_width, "Ready", &lv_font_montserrat_20, LV_TEXT_ALIGN_LEFT);
}

void home_screen_show_upload_confirmation(bool visible)
{
    if (!upload_panel || !home_content) {
        return;
    }
    if (visible) {
        agent_screen_hide();
        notice_screen_hide();
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

void home_screen_set_upload_status(const char *status)
{
    if (upload_status && status && strcmp(lv_label_get_text(upload_status), status) != 0) {
        lv_label_set_text(upload_status, status);
    }
}

void home_screen_set_recording(bool active, const char *status)
{
    if (!recording_icon || !status_label || !status) {
        return;
    }
    if (active == lv_obj_has_flag(recording_icon, LV_OBJ_FLAG_HIDDEN)) {
        if (active) {
            lv_obj_remove_flag(recording_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(recording_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (strcmp(lv_label_get_text(status_label), status) != 0) {
        lv_label_set_text(status_label, status);
    }
}
