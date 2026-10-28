#pragma once

#include <stdbool.h>

// 在 LVGL 初始化完成后，由 UI 任务创建静态占位主页。
// 页面仅负责显示，不注册按键、触摸事件或硬件测试操作。
void home_screen_create(void);

// 仅 UI 任务调用；状态未变化时不触发刷屏，录音图标保持静态。
void home_screen_set_recording(bool active, const char *status);
