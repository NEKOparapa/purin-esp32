#include "lvgl_port.h"

#include "board.h"
#include "epaper_lvgl.h"
#include "touch_lvgl.h"
#include "esp_check.h"

static const char *TAG = "lvgl_port";
static bool initialized;
static touch_lvgl_handle_t touch_adapter;

esp_err_t lvgl_port_init(const lvgl_port_callbacks_t *callbacks)
{
    ESP_RETURN_ON_FALSE(!initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(board_display_init(&panel), TAG, "display hardware init");
    ESP_RETURN_ON_ERROR(epaper_lvgl_init(panel), TAG, "LVGL display init");
    initialized = true;

    // 板级层集中处理供电、引脚、复位和方向；本层只负责接入 LVGL。
    cst816s_touch_handle_t source = NULL;
    board_touch_orientation_t orientation = {0};
    esp_err_t err = board_touch_init(&source, &orientation);
    if (err == ESP_OK) {
        touch_lvgl_config_t config = {
            .source = source,
            .display = lv_display_get_default(),
            .native_width = orientation.native_width,
            .native_height = orientation.native_height,
            .swap_xy = orientation.swap_xy,
            .mirror_x = orientation.mirror_x,
            .mirror_y = orientation.mirror_y,
        };
        if (callbacks) {
            config.callbacks.on_release = callbacks->on_release;
            config.callbacks.on_status = callbacks->on_status;
        }
        err = touch_lvgl_create(&config, &touch_adapter);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Touch unavailable: %s; display remains usable", esp_err_to_name(err));
        if (callbacks && callbacks->on_status) {
            callbacks->on_status(false);
        }
    }
    return ESP_OK;
}

void lvgl_port_request_full_refresh(void)
{
    epaper_lvgl_request_full_refresh();
}
