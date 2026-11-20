#pragma once

#include "lvgl.h"

// Opens the paginated E-Ink Settings Hub (网络 / 连接器 / 接口密钥 / 记忆整理 / 外观 / 信息采集).
void settings_screen_open(lv_obj_t *home_screen);

// Returns to the home screen if any settings page is currently active.
void settings_screen_hide(void);

// Updates live recorder/connection readouts if a settings page is visible.
void settings_screen_refresh_state(void);
