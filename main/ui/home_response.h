#pragma once
#include "lvgl.h"
#include "recorder.h"

void home_response_create(lv_obj_t *parent);
void home_response_refresh(void);
void home_response_set_recorder(recorder_state_t state);
