#pragma once
#include <stddef.h>
#include <stdint.h>

// The calling worker owns the network connection; the SD card is acquired only on a tap.
void notice_save_file(const char *notice_id, const char *file_id, const char *name, int64_t size,
                      char *message, size_t capacity);
