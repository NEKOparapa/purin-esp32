#pragma once

#include "lvgl.h"
#include "recorder.h"

lv_obj_t *status_bar_create(lv_obj_t *parent);
void status_bar_update(recorder_state_t recorder, int battery_percent);
