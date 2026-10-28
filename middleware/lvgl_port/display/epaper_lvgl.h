#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

// 组件内部显示实现：UI 任务调用一次，独立任务负责耗时的 SPI/BUSY 操作。
// panel 为板级层借用句柄；显示工作期间由本适配层独占访问。
esp_err_t epaper_lvgl_init(esp_lcd_panel_handle_t panel);

// UI 任务请求全刷；也可通过复位和全刷重试之前失败的面板操作。
void epaper_lvgl_request_full_refresh(void);
