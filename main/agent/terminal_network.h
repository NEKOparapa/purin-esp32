#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef WINTERMUTE_LOCAL_UPLOAD_CONFIG
#include "upload_config.local.h"
#else
#include "upload_config.example.h"
#endif

bool terminal_configured(void);
esp_err_t terminal_network_init(void);
// Each successful acquire must be paired with a release, on the same worker.
esp_err_t terminal_network_acquire(void);
void terminal_network_release(void);
