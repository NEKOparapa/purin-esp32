#include "home_screen.h"

#include <string.h>
#include "lvgl.h"

static lv_obj_t *recording_icon;
static lv_obj_t *status_label;

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
    make_label(screen, 32, height / 2 - 48, content_width, "HOME", &lv_font_montserrat_48, LV_TEXT_ALIGN_CENTER);
    make_label(screen, 32, height / 2 + 24, content_width, "Home screen placeholder", &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    make_divider(screen, height - 96, content_width);
    status_label = make_label(screen, 32, height - 64, content_width, "Ready", &lv_font_montserrat_20, LV_TEXT_ALIGN_LEFT);
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
