#pragma once

#include <stdint.h>

enum { COVER_WIDTH = 136, COVER_HEIGHT = 176 };

typedef struct {
    uint8_t palette[8];
    uint8_t bits[COVER_HEIGHT][COVER_WIDTH / 8];
} book_cover_t;

// The caller holds the SD mount and owns the returned one-bit thumbnail.
book_cover_t *book_cover_load(const char *book_filename);
