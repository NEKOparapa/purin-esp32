#include "board.h"
#include "device_apps.h"
#include "home_screen.h"
#include "lvgl_port.h"
#include "recorder.h"
#include "recording_button.h"
#include "audio_upload.h"
#include "agent_client.h"
#include "reading_progress.h"
#include "terminal_network.h"
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

    // Set up the event loop before the UI and recorder allocate more internal RAM.
    err = terminal_network_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "Network initialization failed: %s", esp_err_to_name(err));

    // 初始化完成后进入主页；页面与所有 LVGL 调用都由这个任务管理。
    err = device_apps_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "Connector initialization failed: %s", esp_err_to_name(err));
    home_screen_create();
    err = recorder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recorder initialization failed: %s", esp_err_to_name(err));
    }
    err = agent_client_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "Agent initialization failed: %s", esp_err_to_name(err));
    err = audio_upload_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Upload initialization failed: %s", esp_err_to_name(err));
    }
    reading_progress_wake();
    recording_button_t boot = {0};
    recorder_state_t last_reported = RECORDER_IDLE;
    static const char *const state_names[] = {"idle", "starting", "recording", "saving", "error"};
    ESP_LOGI(TAG, "Startup complete: home screen ready");
    for (;;) {
        recording_button_event_t button = recording_button_update(&boot, board_boot_pressed(), (uint64_t)esp_timer_get_time() / 1000);
        if (button == RECORDING_BUTTON_LONG) {
            home_screen_show_upload_confirmation(false);
            recorder_status_t state = recorder_get_status();
            if (state.state == RECORDER_RECORDING) {
                (void)recorder_stop();
            } else if (state.state == RECORDER_IDLE || state.state == RECORDER_ERROR) {
                (void)recorder_start();
            }
        } else if (button == RECORDING_BUTTON_SHORT && recorder_get_status().state == RECORDER_RECORDING && !audio_upload_busy()) {
            if (home_screen_upload_confirmation_visible()) {
                (void)recorder_upload(home_screen_upload_minutes());
                home_screen_show_upload_confirmation(false);
            } else {
                home_screen_show_upload_confirmation(true);
            }
        }
        recorder_status_t status = recorder_get_status();
        if (status.state != last_reported) {
            last_reported = status.state;
            if ((unsigned)status.state < sizeof(state_names) / sizeof(state_names[0])) {
                agent_client_report_recorder_state(state_names[status.state]);
            }
        }
        if (status.state != RECORDER_RECORDING) {
            home_screen_show_upload_confirmation(false);
        }
        home_screen_set_status(status, audio_upload_get_status().state);
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
    // 首页刷新在 UI 任务内完成 LVGL 布局和字形光栅化，需要为渲染预留栈空间。
    if (xTaskCreate(ui_task, "ui", 16 * 1024, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create UI task");
    }
}
