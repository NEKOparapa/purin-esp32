#pragma once

#include "esp_err.h"

esp_err_t terminal_service_init(void);
// Queues the latest recorder state without waiting for connectivity or flash.
void terminal_service_report_recorder_state(const char *state);
