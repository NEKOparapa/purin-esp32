#include <string.h>

#include "display_test_pattern.h"
#include "board.h"
#include "test_apps.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_test";

static esp_err_t write_plane(esp_lcd_panel_handle_t panel, const uint8_t *frame,
                             esp_lcd_ssd1677_bitmap_color_t plane)
{
    ESP_RETURN_ON_ERROR(epaper_panel_set_bitmap_color(panel, plane), TAG, "select plane");
    return esp_lcd_panel_draw_bitmap(panel, 0, 0, SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT, frame);
}

static esp_err_t refresh(esp_lcd_panel_handle_t panel, esp_lcd_ssd1677_refresh_mode_t mode,
                         const char *label)
{
    ESP_RETURN_ON_ERROR(epaper_panel_set_refresh_mode(panel, mode), TAG, "refresh mode");
    int64_t start = esp_timer_get_time();
    // With no refresh callback registered, this port waits for BUSY with a timeout
    // for every mode, including PARTIAL. SPI data is drained before activation.
    ESP_RETURN_ON_ERROR(epaper_panel_refresh_screen(panel), TAG, "%s refresh failed", label);
    ESP_LOGI(TAG, "%s complete (%lld ms)", label, (long long)((esp_timer_get_time() - start) / 1000));
    return ESP_OK;
}

static esp_err_t full_frame(esp_lcd_panel_handle_t panel, const uint8_t *frame, const char *label)
{
    ESP_RETURN_ON_ERROR(write_plane(panel, frame, SSD1677_EPAPER_BITMAP_PREVIOUS), TAG, "previous frame");
    ESP_RETURN_ON_ERROR(write_plane(panel, frame, SSD1677_EPAPER_BITMAP_CURRENT), TAG, "current frame");
    return refresh(panel, SSD1677_EPAPER_REFRESH_FULL, label);
}

static esp_err_t run_test(esp_lcd_panel_handle_t panel, uint8_t *frame, uint8_t *previous)
{
    ESP_LOGI(TAG, "Display test starting");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    vTaskDelay(pdMS_TO_TICKS(100));

    memset(frame, 0xff, SSD1677_PANEL_BUFFER_SIZE);
    ESP_RETURN_ON_ERROR(full_frame(panel, frame, "White screen"), TAG, "white test");
    vTaskDelay(pdMS_TO_TICKS(2000));
    memset(frame, 0x00, SSD1677_PANEL_BUFFER_SIZE);
    ESP_RETURN_ON_ERROR(full_frame(panel, frame, "Black screen"), TAG, "black test");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // The upstream gray LUT is an overlay: paint the black/white base first.
    display_test_draw_gray_base(frame);
    ESP_RETURN_ON_ERROR(full_frame(panel, frame, "Gray chart base"), TAG, "gray base");
    display_test_draw_gray(frame, previous);
    ESP_RETURN_ON_ERROR(write_plane(panel, previous, SSD1677_EPAPER_BITMAP_PREVIOUS), TAG, "gray MSB");
    ESP_RETURN_ON_ERROR(write_plane(panel, frame, SSD1677_EPAPER_BITMAP_CURRENT), TAG, "gray LSB");
    ESP_RETURN_ON_ERROR(refresh(panel, SSD1677_EPAPER_REFRESH_GRAY, "Four gray levels"), TAG, "gray test");
    vTaskDelay(pdMS_TO_TICKS(8000));

    // Reinitialize after gray mode and establish identical BW reference planes.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset after gray");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init after gray");
    display_test_draw_mono(frame);
    ESP_RETURN_ON_ERROR(full_frame(panel, frame, "Monochrome chart"), TAG, "chart test");
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (unsigned count = 1; count <= 5; ++count) {
        memcpy(previous, frame, SSD1677_PANEL_BUFFER_SIZE);
        display_test_draw_counter(frame, count);
        ESP_RETURN_ON_ERROR(epaper_panel_prepare_for_partial(panel), TAG, "partial prepare");
        // Send complete frames, as upstream does. Only the counter pixels change.
        // Explicitly seed the old plane so repeated updates never use stale RAM.
        ESP_RETURN_ON_ERROR(write_plane(panel, previous, SSD1677_EPAPER_BITMAP_PREVIOUS), TAG, "partial old");
        ESP_RETURN_ON_ERROR(write_plane(panel, frame, SSD1677_EPAPER_BITMAP_CURRENT), TAG, "partial new");
        ESP_RETURN_ON_ERROR(refresh(panel, SSD1677_EPAPER_REFRESH_PARTIAL, "Partial update"), TAG, "partial test");
        ESP_LOGI(TAG, "Partial counter %u/5", count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // Stop driving the panel while retaining the last test image for inspection.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, false), TAG, "panel power off");
    ESP_RETURN_ON_ERROR(epaper_panel_deep_sleep(panel), TAG, "panel sleep");
    ESP_LOGI(TAG, "Display test complete. Press BOOT to run again.");
    return ESP_OK;
}

static void wait_for_boot_click(void)
{
    // Require release, then a debounced press and release (no repeats on hold).
    while (board_boot_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (;;) {
        while (!board_boot_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(40));
        if (board_boot_pressed()) {
            while (board_boot_pressed()) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            return;
        }
    }
}

void display_diagnostic_run(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = board_display_init(&panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s. Fix wiring/power and reset.", esp_err_to_name(err));
        return;
    }
    uint8_t *frame = heap_caps_malloc(SSD1677_PANEL_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *previous = heap_caps_malloc(SSD1677_PANEL_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame || !previous) {
        ESP_LOGE(TAG, "Cannot allocate two 48000-byte framebuffers in PSRAM");
        heap_caps_free(frame);
        heap_caps_free(previous);
        return;
    }
    for (;;) {
        err = run_test(panel, frame, previous);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Display test stopped: %s. Check BUSY/power; press BOOT to retry.", esp_err_to_name(err));
        }
        wait_for_boot_click();
    }
}
