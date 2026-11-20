#pragma once

#include "lvgl.h"

// Opens the paginated E-Ink settings (连接 / 网络 / 连接器 / 外观 / 存储 / 信息采集).
void settings_screen_open(lv_obj_t *home_screen);

// Returns to the home screen if any settings page is currently active.
void settings_screen_hide(void);

// Updates live recorder and network readouts.
void settings_screen_refresh_state(void);
