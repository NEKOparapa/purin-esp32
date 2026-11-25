#pragma once
#include "device_apps.h"
#ifndef APP_STORAGE_ROOT
#define APP_STORAGE_ROOT "/sdcard/Purin"
#endif
#define BOOK_DIR APP_STORAGE_ROOT "/Library"
#define AUDIO_DIR APP_STORAGE_ROOT "/Audio"
// Caller owns the board SD mount. Four entries per response, preserving FAT directory order.
typedef bool (*media_name_fn)(const char *name);
typedef cJSON *(*media_entry_fn)(const char *name);
cJSON *media_list(const char *dir, int offset, media_name_fn valid, media_entry_fn entry,
                  char *error, size_t size);
