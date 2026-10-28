#include "epaper_lvgl.h"

#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "epaper_lvgl";
enum { WIDTH = 800, HEIGHT = 480, FRAME_BYTES = WIDTH * HEIGHT / 8,
       PALETTE_BYTES = 8, MAX_PARTIAL_REFRESHES = 10 };

typedef struct {
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
    void *draw_buffer;
    uint32_t draw_stride;
    uint8_t *pending_frame;
    uint8_t *active_frame;
    uint8_t *previous_frame;
    SemaphoreHandle_t pending_lock;
    TaskHandle_t worker;
    // Protected by pending_lock, except force_next_flush (LVGL task only).
    bool pending;
    bool pending_full;
    bool force_next_flush;
} epaper_context_t;

static epaper_context_t ctx;

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t reset_panel(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(ctx.panel), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(ctx.panel), TAG, "panel init");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t write_plane(const uint8_t *frame, esp_lcd_ssd1677_bitmap_color_t plane)
{
    ESP_RETURN_ON_ERROR(epaper_panel_set_bitmap_color(ctx.panel, plane), TAG, "select plane");
    return esp_lcd_panel_draw_bitmap(ctx.panel, 0, 0, WIDTH, HEIGHT, frame);
}

static esp_err_t refresh_panel(bool full)
{
    if (!full) {
        ESP_RETURN_ON_ERROR(epaper_panel_prepare_for_partial(ctx.panel), TAG, "partial prepare");
    }
    ESP_RETURN_ON_ERROR(write_plane(full ? ctx.active_frame : ctx.previous_frame,
                                    SSD1677_EPAPER_BITMAP_PREVIOUS), TAG, "previous plane");
    ESP_RETURN_ON_ERROR(write_plane(ctx.active_frame, SSD1677_EPAPER_BITMAP_CURRENT), TAG, "current plane");
    ESP_RETURN_ON_ERROR(epaper_panel_set_refresh_mode(ctx.panel, full ? SSD1677_EPAPER_REFRESH_FULL
                                                                    : SSD1677_EPAPER_REFRESH_PARTIAL),
                        TAG, "refresh mode");
    // No ISR callback is registered: all refresh modes wait synchronously here.
    return epaper_panel_refresh_screen(ctx.panel);
}

static void refresh_worker(void *arg)
{
    (void)arg;
    bool have_previous = false;
    bool needs_reset = false;
    unsigned partial_count = 0;
    unsigned completed = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Coalesce bursts of UI changes, including those received during BUSY.
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(ctx.pending_lock, portMAX_DELAY);
        if (!ctx.pending) {
            xSemaphoreGive(ctx.pending_lock);
            continue;
        }
        memcpy(ctx.active_frame, ctx.pending_frame, FRAME_BYTES);
        const bool requested_full = ctx.pending_full;
        ctx.pending = false;
        ctx.pending_full = false;
        xSemaphoreGive(ctx.pending_lock);

        const bool full = requested_full || !have_previous || partial_count >= MAX_PARTIAL_REFRESHES;
        if (!requested_full && have_previous &&
            memcmp(ctx.active_frame, ctx.previous_frame, FRAME_BYTES) == 0) {
            continue;
        }
        int64_t start = esp_timer_get_time();
        esp_err_t err = needs_reset ? reset_panel() : ESP_OK;
        if (err == ESP_OK) {
            err = refresh_panel(full);
        }
        if (err != ESP_OK) {
            // The visible image and controller RAM are now uncertain. Do not
            // promote this frame to previous or spin retrying a failed panel.
            have_previous = false;
            needs_reset = true;
            partial_count = 0;
            ESP_LOGE(TAG, "Refresh failed: %s. Next UI update / long BOOT retries with full refresh.",
                     esp_err_to_name(err));
            continue;
        }
        memcpy(ctx.previous_frame, ctx.active_frame, FRAME_BYTES);
        have_previous = true;
        needs_reset = false;
        partial_count = full ? 0 : partial_count + 1;
        ESP_LOGI(TAG, "Frame %u %s complete (%lld ms)", ++completed, full ? "FULL" : "PARTIAL",
                 (long long)((esp_timer_get_time() - start) / 1000));
    }
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    // FULL render mode gives exactly one full-screen I1 buffer, with two
    // 32-bit palette entries followed by MSB-first pixels (1=white, 0=black).
    if (area->x1 != 0 || area->y1 != 0 || area->x2 != WIDTH - 1 || area->y2 != HEIGHT - 1) {
        ESP_LOGE(TAG, "Unexpected LVGL flush area; this port requires FULL render mode");
        lv_display_flush_ready(display);
        return;
    }
    xSemaphoreTake(ctx.pending_lock, portMAX_DELAY);
    const uint8_t *src = pixels + PALETTE_BYTES;
    for (int y = 0; y < HEIGHT; ++y) {
        memcpy(ctx.pending_frame + y * (WIDTH / 8), src + y * ctx.draw_stride, WIDTH / 8);
    }
    ctx.pending = true;
    // A later coalesced frame must not erase an earlier full-refresh request.
    ctx.pending_full |= ctx.force_next_flush;
    ctx.force_next_flush = false;
    xSemaphoreGive(ctx.pending_lock);
    xTaskNotifyGive(ctx.worker);
    // Only the copied snapshot is used by the worker. LVGL can reuse its draw
    // buffer immediately, without waiting for the multi-second panel waveform.
    lv_display_flush_ready(display);
}

esp_err_t epaper_lvgl_init(esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_FALSE(panel && !ctx.display, ESP_ERR_INVALID_STATE, TAG, "invalid panel / already initialized");
    ctx.panel = panel;
    ESP_RETURN_ON_ERROR(reset_panel(), TAG, "SSD1677 init");
    lv_init();
    lv_tick_set_cb(tick_ms);
    ctx.draw_stride = lv_draw_buf_width_to_stride(WIDTH, LV_COLOR_FORMAT_I1);
    const size_t draw_size = ctx.draw_stride * HEIGHT + PALETTE_BYTES;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t align = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);
    ctx.draw_buffer = heap_caps_aligned_alloc(align, draw_size, caps);
    ctx.pending_frame = heap_caps_malloc(FRAME_BYTES, caps);
    ctx.active_frame = heap_caps_malloc(FRAME_BYTES, caps);
    ctx.previous_frame = heap_caps_malloc(FRAME_BYTES, caps);
    ctx.pending_lock = xSemaphoreCreateMutex();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!ctx.draw_buffer || !ctx.pending_frame || !ctx.active_frame || !ctx.previous_frame || !ctx.pending_lock) {
        goto fail;
    }
    memset(ctx.draw_buffer, 0xff, draw_size);
    ctx.display = lv_display_create(WIDTH, HEIGHT);
    if (!ctx.display) {
        goto fail;
    }
    lv_display_set_color_format(ctx.display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(ctx.display, ctx.draw_buffer, NULL, draw_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(ctx.display, flush_cb);
    // Internal task stack: panel operations must not run with a PSRAM stack.
    if (xTaskCreate(refresh_worker, "epaper_refresh", 4096, NULL, 2, &ctx.worker) != pdPASS) {
        goto fail;
    }
    ESP_LOGI(TAG, "LVGL %d.%d.%d ready: 800x480 I1, async panel worker",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;

fail:
    if (ctx.display) {
        lv_display_delete(ctx.display);
    }
    if (ctx.pending_lock) {
        vSemaphoreDelete(ctx.pending_lock);
    }
    heap_caps_free(ctx.draw_buffer);
    heap_caps_free(ctx.pending_frame);
    heap_caps_free(ctx.active_frame);
    heap_caps_free(ctx.previous_frame);
    memset(&ctx, 0, sizeof(ctx));
    ESP_LOGE(TAG, "Cannot allocate LVGL display / panel worker");
    return err;
}

void epaper_lvgl_request_full_refresh(void)
{
    if (ctx.display) {
        ctx.force_next_flush = true;
        lv_obj_invalidate(lv_display_get_screen_active(ctx.display));
    }
}
