#include "lvgl_demo_ui.h"

#include "lvgl.h"

static lv_obj_t *next_button;
static lv_obj_t *counter_label;
static lv_obj_t *level_label;
static lv_obj_t *level_bar;
static lv_obj_t *touch_status_label;
static lv_obj_t *coordinates_label;
static lv_obj_t *sd_status_label;
static lv_obj_t *vibration_status_label;
static unsigned counter;
static bool touch_available;
static const char *last_target = "--";
static void (*full_refresh_cb)(void);
static void (*sd_test_cb)(void);
static void (*vibration_cb)(uint32_t duration_ms);
static const uint32_t vibration_durations[] = {35, 200, 0};

/* 显式黑白样式：去掉主题渐变、按下动画和滚动，减少墨水屏无意义刷新。 */
static void plain_object(lv_obj_t *obj, int x, int y, int width, int height)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width,
                       const char *value, const lv_font_t *font)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(obj, value);
    return obj;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    plain_object(obj, x, y, width, height);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                        const char *id, const char *title, bool filled)
{
    lv_obj_t *obj = lv_button_create(parent);
    plain_object(obj, x, y, width, height);
    /* 稳定字符串 ID 用于主机测试查找控件，不依赖创建顺序。 */
    lv_obj_set_user_data(obj, (void *)id);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_t *caption = label(obj, 4, 0, width - 12, title, &lv_font_montserrat_20);
    lv_obj_set_style_text_color(caption, filled ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(caption);
    return obj;
}

static void update_counter(void)
{
    const unsigned level = (counter % 11) * 10;
    lv_label_set_text_fmt(counter_label, "%04u", counter);
    lv_label_set_text_fmt(level_label, "LEVEL %u%%", level);
    lv_bar_set_value(level_bar, (int32_t)level, LV_ANIM_OFF);
}

static void update_touch_status(void)
{
    if (!touch_status_label) {
        return;
    }
    if (touch_available) {
        lv_label_set_text_fmt(touch_status_label, "TOUCH: READY   /   LAST TARGET: %s", last_target);
    } else {
        lv_label_set_text(touch_status_label, "NO TOUCH   /   TAP TO WAKE; BOOT STILL WORKS");
    }
}

static void on_next_clicked(lv_event_t *event)
{
    (void)event;
    counter = (counter + 1) % 10000;
    update_counter();
}

static void on_reset_clicked(lv_event_t *event)
{
    (void)event;
    counter = 0;
    update_counter();
}

static void on_full_refresh_clicked(lv_event_t *event)
{
    (void)event;
    if (full_refresh_cb) {
        full_refresh_cb();
    }
}

static void on_corner_clicked(lv_event_t *event)
{
    last_target = lv_event_get_user_data(event);
    update_touch_status();
}

static void on_sd_clicked(lv_event_t *event)
{
    (void)event;
    if (sd_test_cb) {
        sd_test_cb();
    }
}

static void on_vibration_clicked(lv_event_t *event)
{
    const uint32_t *duration_ms = lv_event_get_user_data(event);
    if (vibration_cb && duration_ms) {
        // 只提交请求，定时关断由底层执行；不在 LVGL 回调中等待震动结束。
        vibration_cb(*duration_ms);
    }
}

static void corner(lv_obj_t *screen, int x, int y, const char *id, const char *name)
{
    lv_obj_t *obj = button(screen, x, y, 64, 48, id, name, false);
    lv_obj_add_event_cb(obj, on_corner_clicked, LV_EVENT_CLICKED, (void *)name);
}

void lvgl_demo_ui_create(void)
{
    counter = 0;
    last_target = "--";
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    plain_object(screen, 0, 0, 800, 480);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_20, LV_PART_MAIN);

    label(screen, 92, 14, 616, "WINTERMUTE / HARDWARE TEST", &lv_font_montserrat_28);
    label(screen, 92, 54, 616, "DISPLAY / TOUCH / SD / VIBRATION", &lv_font_montserrat_20);
    lv_obj_t *divider = lv_obj_create(screen);
    plain_object(divider, 24, 88, 752, 2);
    lv_obj_set_style_bg_color(divider, lv_color_black(), LV_PART_MAIN);
    touch_status_label = label(screen, 24, 100, 752, "", &lv_font_montserrat_20);
    lv_obj_set_user_data(touch_status_label, "touch_status");
    update_touch_status();

    lv_obj_t *count_card = card(screen, 24, 134, 220, 100);
    label(count_card, 14, 10, 188, "BUTTON EVENTS", &lv_font_montserrat_20);
    counter_label = label(count_card, 12, 42, 192, "0000", &lv_font_montserrat_48);
    lv_obj_set_user_data(counter_label, "counter");
    lv_obj_set_style_text_align(counter_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_t *widgets = card(screen, 268, 134, 508, 100);
    level_label = label(widgets, 20, 10, 464, "LEVEL 0%", &lv_font_montserrat_20);
    lv_obj_set_user_data(level_label, "level_text");
    level_bar = lv_bar_create(widgets);
    plain_object(level_bar, 20, 40, 464, 20);
    lv_obj_set_user_data(level_bar, "level_bar");
    lv_obj_set_style_border_color(level_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(level_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(level_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(level_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(level_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(level_bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(level_bar, 0, 100);
    lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
    lv_obj_t *sd_button = button(widgets, 20, 64, 464, 28,
                                 "sd_test", "SD: CHECKING...", false);
    sd_status_label = lv_obj_get_child(sd_button, 0);
    lv_obj_set_user_data(sd_status_label, "sd_status");
    lv_obj_add_event_cb(sd_button, on_sd_clicked, LV_EVENT_CLICKED, NULL);

    next_button = button(screen, 24, 246, 236, 48, "next", "NEXT +1", true);
    lv_obj_add_event_cb(next_button, on_next_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reset_button = button(screen, 282, 246, 236, 48, "reset", "RESET", false);
    lv_obj_add_event_cb(reset_button, on_reset_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *full_button = button(screen, 540, 246, 236, 48, "full", "FULL REFRESH", false);
    lv_obj_add_event_cb(full_button, on_full_refresh_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vibe_short = button(screen, 24, 306, 236, 48, "vibe_short", "VIBE 35ms", false);
    lv_obj_add_event_cb(vibe_short, on_vibration_clicked, LV_EVENT_CLICKED, (void *)&vibration_durations[0]);
    lv_obj_t *vibe_long = button(screen, 282, 306, 236, 48, "vibe_long", "VIBE 200ms", false);
    lv_obj_add_event_cb(vibe_long, on_vibration_clicked, LV_EVENT_CLICKED, (void *)&vibration_durations[1]);
    lv_obj_t *vibe_stop = button(screen, 540, 306, 236, 48, "vibe_stop", "STOP", false);
    lv_obj_add_event_cb(vibe_stop, on_vibration_clicked, LV_EVENT_CLICKED, (void *)&vibration_durations[2]);
    vibration_status_label = label(screen, 24, 366, 752, "VIBE: INITIALIZING...", &lv_font_montserrat_20);
    lv_obj_set_user_data(vibration_status_label, "vibe_status");

    coordinates_label = label(screen, 92, 402, 616, "RAW --,--  >  SCREEN --,--", &lv_font_montserrat_20);
    lv_obj_set_user_data(coordinates_label, "coordinates");
    label(screen, 92, 442, 616, "BOOT: next + vibe; hold 1s: refresh", &lv_font_montserrat_20);
    /* 四角测试块覆盖不同屏幕边缘，便于判断交换坐标轴或镜像错误。 */
    corner(screen, 8, 8, "corner_tl", "TL");
    corner(screen, 728, 8, "corner_tr", "TR");
    corner(screen, 8, 424, "corner_bl", "BL");
    corner(screen, 728, 424, "corner_br", "BR");
}

void lvgl_demo_ui_set_full_refresh_cb(void (*cb)(void))
{
    full_refresh_cb = cb;
}

void lvgl_demo_ui_set_sd_test_cb(void (*cb)(void))
{
    sd_test_cb = cb;
}

void lvgl_demo_ui_set_sd_status(const char *status)
{
    if (sd_status_label && status) {
        lv_label_set_text(sd_status_label, status);
    }
}

void lvgl_demo_ui_set_vibration_cb(void (*cb)(uint32_t duration_ms))
{
    vibration_cb = cb;
}

void lvgl_demo_ui_set_vibration_status(const char *status)
{
    if (vibration_status_label && status) {
        lv_label_set_text(vibration_status_label, status);
    }
}

void lvgl_demo_ui_set_touch_available(bool available)
{
    touch_available = available;
    update_touch_status();
}

void lvgl_demo_ui_report_touch(uint16_t raw_x, uint16_t raw_y, uint16_t x, uint16_t y)
{
    if (coordinates_label) {
        lv_label_set_text_fmt(coordinates_label, "RAW %u,%u  >  SCREEN %u,%u",
                             (unsigned)raw_x, (unsigned)raw_y, (unsigned)x, (unsigned)y);
    }
}

void lvgl_demo_ui_advance(void)
{
    if (next_button != NULL) {
        lv_obj_send_event(next_button, LV_EVENT_CLICKED, NULL);
    }
}
