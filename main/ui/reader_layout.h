#pragma once

#include "lvgl.h"

typedef struct {
    uint32_t *starts;
    uint32_t count;
} reader_layout_t;

// The font belongs to the calling task. Normalize tabs without changing byte offsets.
bool reader_layout_build(reader_layout_t *layout, char *text, uint32_t length,
                         const lv_font_t *font, int width, int lines);
char *reader_layout_text(const reader_layout_t *layout, const char *text, uint32_t page);
