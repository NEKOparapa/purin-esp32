#include "reader_layout.h"
#include "src/misc/lv_text_private.h"
#include <stdlib.h>

bool reader_layout_build(reader_layout_t *layout, char *text, uint32_t length,
                         const lv_font_t *font, int width, int lines)
{
    *layout = (reader_layout_t){0};
    if (!length || width <= 0 || lines <= 0) return false;
    uint32_t *starts = malloc((length / lines + 2) * sizeof(*starts));
    if (!starts) return false;
    for (uint32_t i = 0; i < length; ++i)
        if (text[i] == '\t') text[i] = ' ';

    lv_text_attributes_t attrs = {.max_width = width, .text_flags = LV_TEXT_FLAG_NONE};
    uint32_t at = 0;
    while (at < length) {
        starts[layout->count++] = at;
        for (int line = 0; line < lines && at < length; ++line) {
            uint32_t step = lv_text_get_next_line(text + at, length - at, font, NULL, &attrs);
            if (!step || step > length - at) {
                free(starts);
                *layout = (reader_layout_t){0};
                return false;
            }
            at += step;
            if (at < length && text[at - 1] == '\r' && text[at] == '\n') ++at;
        }
    }
    starts[layout->count] = at;
    layout->starts = starts;
    return true;
}

char *reader_layout_text(const reader_layout_t *layout, const char *text, uint32_t page)
{
    uint32_t start = layout->starts[page], end = layout->starts[page + 1];
    // A terminal break ends this page and must not add a line to the next one.
    if (end > start && text[end - 1] == '\n') --end;
    if (end > start && text[end - 1] == '\r') --end;
    char *copy = malloc(end - start + 1);
    if (!copy) return NULL;
    uint32_t size = 0;
    for (uint32_t i = start; i < end; ++i) {
        if (text[i] == '\r' && i + 1 < end && text[i + 1] == '\n') continue;
        copy[size++] = text[i] == '\r' ? '\n' : text[i];
    }
    copy[size] = '\0';
    return copy;
}
