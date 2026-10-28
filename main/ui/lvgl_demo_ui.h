#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 所有接口都在运行 lv_timer_handler() 的 UI 任务中调用。
 * 使用当前的 800×480 横屏显示；触摸采样和 LVGL 输入设备由调用方注册。
 */
void lvgl_demo_ui_create(void);

/* BOOT 短按复用屏幕 NEXT 按钮的 LV_EVENT_CLICKED 事件。 */
void lvgl_demo_ui_advance(void);

/* 注入全刷操作。界面不直接依赖面板驱动，可在 create 前或后设置。 */
void lvgl_demo_ui_set_full_refresh_cb(void (*cb)(void));

/* SD 测试按钮只请求后台任务；状态文本由 UI 任务读取测试结果后更新。 */
void lvgl_demo_ui_set_sd_test_cb(void (*cb)(void));
void lvgl_demo_ui_set_sd_status(const char *status);

/* 更新触摸硬件状态；不可用时仍保留 BOOT 操作。 */
void lvgl_demo_ui_set_touch_available(bool available);

/* 仅在一次屏内触摸释放后调用，避免每个采样点都触发墨水屏刷新。
 * raw 为 CST816S 原生坐标，x/y 为转换后的横屏坐标。
 */
void lvgl_demo_ui_report_touch(uint16_t raw_x, uint16_t raw_y, uint16_t x, uint16_t y);
