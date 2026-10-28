#include "test_apps.h"

#include <inttypes.h>
#include <stdio.h>
#include "board.h"
#include "lvgl_port.h"
#include "lvgl_demo_ui.h"
#include "sdcard_test.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hardware_test";
static bool vibration_pending;

static void request_vibration(uint32_t duration_ms)
{
    // UI 约定 0 为停止；默认脉宽由明确的 35 ms 按钮测试，不与驱动的 0 语义混用。
    esp_err_t err = duration_ms ? board_vibration_pulse(duration_ms) : board_vibration_stop();
    if (err != ESP_OK) {
        vibration_pending = false;
        lvgl_demo_ui_set_vibration_status("VIBE: ERROR / SEE LOG");
        ESP_LOGW(TAG, "Vibration %s failed: %s", duration_ms ? "pulse" : "stop", esp_err_to_name(err));
        return;
    }
    vibration_pending = duration_ms != 0;
    if (duration_ms) {
        char status[64];
        snprintf(status, sizeof(status), "VIBE: %" PRIu32 " ms REQUESTED", duration_ms);
        lvgl_demo_ui_set_vibration_status(status);
        ESP_LOGI(TAG, "Vibration test: %" PRIu32 " ms requested; confirm motor by touch", duration_ms);
    } else {
        lvgl_demo_ui_set_vibration_status("VIBE: STOPPED");
        ESP_LOGI(TAG, "Vibration test: stopped");
    }
}

static void update_test_status(void)
{
    if (vibration_pending && !board_vibration_is_active()) {
        vibration_pending = false;
        lvgl_demo_ui_set_vibration_status("VIBE: OUTPUT OFF / CHECK FEEL");
        ESP_LOGI(TAG, "Vibration test: timed output OFF (no motor feedback sensor)");
    }
    sdcard_test_result_t result;
    if (!sdcard_test_poll(&result)) {
        return;
    }
    char status[80];
    switch (result.state) {
    case SDCARD_TEST_PASS:
        snprintf(status, sizeof(status), "SD: PASS / %" PRIu64 " MiB / TAP TO RETEST",
                 result.capacity_bytes / (1024 * 1024));
        break;
    case SDCARD_TEST_MOUNT_FAILED:
        snprintf(status, sizeof(status), "SD: NOT READY / TAP TO RETRY");
        break;
    case SDCARD_TEST_IO_FAILED:
        snprintf(status, sizeof(status), "SD: TEST FAILED / TAP TO RETRY");
        break;
    default:
        snprintf(status, sizeof(status), "SD: CHECKING...");
        break;
    }
    lvgl_demo_ui_set_sd_status(status);
}

static void ui_task(void *arg)
{
    (void)arg;
    const lvgl_port_callbacks_t callbacks = {
        .on_release = lvgl_demo_ui_report_touch,
        .on_status = lvgl_demo_ui_set_touch_available,
    };
    esp_err_t err = lvgl_port_init(&callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    lvgl_demo_ui_create();
    lvgl_demo_ui_set_full_refresh_cb(lvgl_port_request_full_refresh);
    lvgl_demo_ui_set_touch_available(false);
    lvgl_demo_ui_set_vibration_cb(request_vibration);
    err = board_vibration_init();
    lvgl_demo_ui_set_vibration_status(err == ESP_OK ? "VIBE: READY / TAP TO TEST" : "VIBE: INIT FAILED");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Vibration init failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "LVGL demo ready. BOOT: click updates + 35 ms vibration; hold 1 second for full refresh.");
    ESP_LOGI(TAG, "Touch test ready: tap NEXT / RESET / FULL REFRESH and four corner targets.");
    ESP_LOGI(TAG, "Vibration test ready: 35 ms / 200 ms / STOP; BOOT click also requests 35 ms.");
    lvgl_demo_ui_set_sd_test_cb(sdcard_test_request);
    err = sdcard_test_start();
    if (err != ESP_OK) {
        lvgl_demo_ui_set_sd_status("SD: TASK START FAILED");
        ESP_LOGW(TAG, "SD task failed: %s", esp_err_to_name(err));
    }

    int raw = !board_boot_pressed();
    int stable = raw;
    int64_t changed_at = esp_timer_get_time();
    int64_t pressed_at = 0;
    bool armed = raw != 0;
    bool long_sent = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int level = !board_boot_pressed();
        if (level != raw) {
            raw = level;
            changed_at = now;
        }
        if (raw != stable && now - changed_at >= 40000) {
            stable = raw;
            if (!stable && armed) {
                pressed_at = now;
                long_sent = false;
            } else if (stable) {
                if (armed && pressed_at && !long_sent) {
                    lvgl_demo_ui_advance();
                    request_vibration(35); // 无触摸时也能人工验证短震，保留计数操作。
                    ESP_LOGI(TAG, "BOOT click: counter updated");
                }
                armed = true;
                pressed_at = 0;
            }
        }
        if (armed && !stable && pressed_at && !long_sent && now - pressed_at >= 1000000) {
            lvgl_port_request_full_refresh();
            long_sent = true;
            ESP_LOGI(TAG, "BOOT hold: full refresh requested");
        }
        update_test_status();
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
    }
}

void hardware_test_run(void)
{
    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(ui_task, "lvgl_ui", 8192, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create LVGL UI task");
    }
}
