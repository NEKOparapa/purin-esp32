#include "touch_lvgl.h"

#include <stdlib.h>
#include "esp_check.h"
#include "touch_transform.h"

static const char *TAG = "touch_lvgl";

struct touch_lvgl {
    touch_lvgl_config_t config;
    lv_indev_t *indev;
    cst816s_touch_sample_t delivered;
    uint16_t x, y;
    bool suppress_until_release;
};

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_lvgl_handle_t adapter = lv_indev_get_user_data(indev);
    cst816s_touch_sample_t sample;
    if (cst816s_touch_read(adapter->config.source, &sample)) {
        if (!sample.down) {
            adapter->suppress_until_release = false;
        } else if (adapter->suppress_until_release ||
                   !touch_transform_point(sample.raw_x, sample.raw_y,
                                          adapter->config.native_width, adapter->config.native_height,
                                          adapter->config.swap_xy, adapter->config.mirror_x,
                                          adapter->config.mirror_y, &adapter->x, &adapter->y)) {
            sample.down = false;
            sample.cancel = true;
            adapter->suppress_until_release = true;
        }
        if (sample.cancel) {
            // 丢失状态时取消 LVGL 手势，不能把通信失败变成正常松手点击。
            lv_indev_reset(indev, NULL);
        } else if (adapter->delivered.down && !sample.down && adapter->config.callbacks.on_release) {
            adapter->config.callbacks.on_release(sample.raw_x, sample.raw_y, adapter->x, adapter->y);
            ESP_LOGI(TAG, "Touch release raw=(%u,%u) screen=(%u,%u)",
                     sample.raw_x, sample.raw_y, adapter->x, adapter->y);
        }
        if (sample.ready != adapter->delivered.ready && adapter->config.callbacks.on_status) {
            adapter->config.callbacks.on_status(sample.ready);
        }
        adapter->delivered = sample;
    }
    data->point.x = adapter->x;
    data->point.y = adapter->y;
    data->state = adapter->delivered.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    // 顺序消费每个按下/松手，保留两次 LVGL 轮询之间的快速点击。
    data->continue_reading = cst816s_touch_has_pending(adapter->config.source);
}

esp_err_t touch_lvgl_create(const touch_lvgl_config_t *config, touch_lvgl_handle_t *out_adapter)
{
    ESP_RETURN_ON_FALSE(config && out_adapter, ESP_ERR_INVALID_ARG, TAG, "NULL config/output");
    *out_adapter = NULL;
    ESP_RETURN_ON_FALSE(config->source && config->display && config->native_width && config->native_height,
                        ESP_ERR_INVALID_ARG, TAG, "missing source, display or geometry");
    const uint16_t width = config->swap_xy ? config->native_height : config->native_width;
    const uint16_t height = config->swap_xy ? config->native_width : config->native_height;
    ESP_RETURN_ON_FALSE(width == lv_display_get_horizontal_resolution(config->display) &&
                        height == lv_display_get_vertical_resolution(config->display),
                        ESP_ERR_INVALID_ARG, TAG, "touch/display resolution mismatch");
    touch_lvgl_handle_t adapter = calloc(1, sizeof(*adapter));
    ESP_RETURN_ON_FALSE(adapter, ESP_ERR_NO_MEM, TAG, "adapter allocation failed");
    adapter->config = *config;
    adapter->indev = lv_indev_create();
    if (!adapter->indev) {
        free(adapter);
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(adapter->indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(adapter->indev, config->display);
    lv_indev_set_user_data(adapter->indev, adapter);
    lv_indev_set_read_cb(adapter->indev, touch_read_cb);
    lv_timer_set_period(lv_indev_get_read_timer(adapter->indev), 10);
    *out_adapter = adapter;
    return ESP_OK;
}

void touch_lvgl_delete(touch_lvgl_handle_t adapter)
{
    if (adapter) {
        lv_indev_delete(adapter->indev);
        free(adapter);
    }
}
