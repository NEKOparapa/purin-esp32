#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 回调均在调用 lv_timer_handler() 的 UI 任务执行，可直接修改控件。
    // on_release 只报告正常松手；通信失败、越界或队列溢出会取消手势。
    void (*on_release)(uint16_t raw_x, uint16_t raw_y, uint16_t x, uint16_t y);
    void (*on_status)(bool available);
} lvgl_port_callbacks_t;

// UI 任务中调用一次：通过 board 初始化显示与触摸，注册 LVGL 显示和输入设备。
// 不创建页面；成功后由应用创建控件并在同一任务循环调用 lv_timer_handler()。
// callbacks 可为 NULL；内容会复制。触摸不可用时报告状态并继续显示，ESP_OK
// 不代表触摸芯片已响应，休眠中的芯片会在后续触摸中断到来时重新探测。
// 初始化期间可能同步调用 on_status(false)，应用回调应允许页面尚未创建。
esp_err_t lvgl_port_init(const lvgl_port_callbacks_t *callbacks);

// UI 任务中调用；请求全刷当前画面，实际 SPI/BUSY 等待在独立刷新任务执行。
void lvgl_port_request_full_refresh(void);

#ifdef __cplusplus
}
#endif
