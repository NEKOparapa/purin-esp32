#include "board.h"
#include "home_screen.h"
#include "lvgl_port.h"
#include "recorder.h"
#include "recording_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

static void ui_task(void *arg)
{
    (void)arg;
    esp_err_t err = lvgl_port_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    // 初始化完成后进入主页；页面与所有 LVGL 调用都由这个任务管理。
    home_screen_create();
    err = recorder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recorder initialization failed: %s", esp_err_to_name(err));
    }
    recording_button_t boot = {0};
    ESP_LOGI(TAG, "Startup complete: home screen ready");
    for (;;) {
        if (recording_button_update(&boot, board_boot_pressed(), (uint64_t)esp_timer_get_time() / 1000)) {
            recorder_status_t state = recorder_get_status();
            if (state.state == RECORDER_RECORDING) {
                (void)recorder_stop();
            } else if (state.state == RECORDER_IDLE || state.state == RECORDER_ERROR) {
                (void)recorder_start();
            }
        }
        recorder_status_t status = recorder_get_status();
        const char *text = recorder_error_text(status.error);
        if (status.state == RECORDER_STARTING) {
            text = "Starting recording";
        } else if (status.state == RECORDER_RECORDING) {
            text = "Recording";
        } else if (status.state == RECORDER_STOPPING) {
            text = "Saving";
        }
        home_screen_set_recording(status.state == RECORDER_RECORDING || status.state == RECORDER_STOPPING, text);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
    }
}

void app_main(void)
{
    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Board initialization failed: %s", esp_err_to_name(err));
        return;
    }
    // 保留独立 UI 任务的栈空间，不在默认 main 任务栈上执行 LVGL 布局。
    if (xTaskCreate(ui_task, "ui", 8192, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create UI task");
    }
}
