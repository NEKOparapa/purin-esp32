/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified for purin-esp32; see NOTICE.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 刷新完成 ISR 回调类型 */
typedef bool (*esp_lcd_epaper_panel_cb_t)(const esp_lcd_panel_handle_t handle, const void *edata, void *user_data);

typedef struct {
    esp_lcd_epaper_panel_cb_t on_epaper_refresh_done; // 刷新结束回调
} epaper_panel_callbacks_t;

typedef struct {
    int busy_gpio_num;         // BUSY 脚
    bool non_copy_mode;        // true：直接用调用方缓冲，不做内部拷贝
    bool use_fast_full_update; // true：允许快速全刷模式
} esp_lcd_ssd1677_config_t;

typedef enum {
    SSD1677_EPAPER_BITMAP_CURRENT = 0, // 写 0x24 当前帧
    SSD1677_EPAPER_BITMAP_PREVIOUS,    // 写 0x26 上一帧
} esp_lcd_ssd1677_bitmap_color_t;

typedef enum {
    SSD1677_EPAPER_REFRESH_FULL = 0,  // 全刷（旧 0xC7 / 优化 0xC4）
    SSD1677_EPAPER_REFRESH_FULL_FAST, // 快速全刷
    SSD1677_EPAPER_REFRESH_PARTIAL,   // 局刷（旧 0xCF / 优化 0xCC）
    SSD1677_EPAPER_REFRESH_STANDBY,   // 进/出待机与关机（同全刷档）
    SSD1677_EPAPER_REFRESH_GRAY,      // 0x22=0xCC + 外置灰 LUT（同步等 BUSY）
} esp_lcd_ssd1677_refresh_mode_t;

/**
 * @brief 创建 SSD1677 墨水屏 panel
 * @param io panel IO 句柄
 * @param panel_dev_config 设备配置（vendor_config 指向 esp_lcd_ssd1677_config_t）
 * @param ret_panel 输出 panel 句柄
 * @return ESP_OK 成功
 */
esp_err_t esp_lcd_new_panel_ssd1677(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Refresh the panel; a BUSY wait times out after 15 seconds.
 * FULL/GRAY are synchronous. PARTIAL is also synchronous unless a non-null
 * refresh callback has been registered, in which case completion is asynchronous.
 * A 100 us delay after activation allows BUSY to become active before polling.
 * @note MCU LUT 仅在全↔局切换时重下
 */
esp_err_t epaper_panel_refresh_screen(esp_lcd_panel_t *panel);

/**
 * @brief Wait for BUSY to go low, with a 15 second timeout.
 * This waits for panel refresh, not queued SPI DMA. Do not modify a bitmap
 * while its asynchronous draw transfer is still in progress. A subsequent
 * refresh command drains queued SPI transfers before activating the panel.
 */
esp_err_t epaper_panel_wait_ready(esp_lcd_panel_t *panel);

/**
 * @brief 设置下次刷新模式
 */
esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_t *panel, esp_lcd_ssd1677_refresh_mode_t mode);

/**
 * @brief 设置 draw_bitmap 目标平面（0x24 / 0x26）
 */
esp_err_t epaper_panel_set_bitmap_color(esp_lcd_panel_t *panel, esp_lcd_ssd1677_bitmap_color_t color);

/**
 * @brief Register the ISR callback for asynchronous PARTIAL refresh.
 * Install the GPIO ISR service before registering a non-null callback.
 * Pass a callbacks struct with a null callback to restore synchronous partials.
 * Register/unregister only while the panel is idle. FULL/GRAY never call this.
 */
esp_err_t epaper_panel_register_event_callbacks(esp_lcd_panel_t *panel, epaper_panel_callbacks_t *cbs, void *user_ctx);

/* draw_bitmap uses tightly packed, MSB-first 1bpp rows, 1=white / 0=black.
 * Both x coordinates must be multiples of 8 and all coordinates must be within
 * 800x480. With non_copy_mode=true, inversion is not performed by the driver.
 */

/**
 * @brief 每次局刷前：写边框 0x80（无 HW RST / soft reset）
 */
esp_err_t epaper_panel_prepare_for_partial(esp_lcd_panel_t *panel);

/**
 * @brief 进入 Deep Sleep（0x10）；唤醒需硬件 RST
 */
esp_err_t epaper_panel_deep_sleep(esp_lcd_panel_t *panel);

#ifdef __cplusplus
}
#endif
