#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "recorder.h"

// 在 LVGL 初始化完成后，由 UI 任务创建主页。
// 页面管理触摸选择，BOOT 操作由应用层负责。
void home_screen_create(void);

// 仅 UI 任务调用；状态未变化时不触发刷屏，录音图标保持静态。
void home_screen_set_status(recorder_state_t recorder, int battery_percent);

void home_screen_show_upload_confirmation(bool visible);
bool home_screen_upload_confirmation_visible(void);
uint32_t home_screen_upload_minutes(void);
