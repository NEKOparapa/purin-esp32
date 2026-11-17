#pragma once

#include <stdbool.h>
#include "nvs.h"

bool terminal_tools_enabled(void);
// Invoked only by the Agent network worker, outside the LVGL thread.
void terminal_tools_poll(nvs_handle_t journal);
