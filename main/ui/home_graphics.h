#pragma once

#include "lvgl.h"

// Fixed home controls use solid one-bit glyphs on either ink or paper.
LV_FONT_DECLARE(home_controls);

enum { HOME_RULE_WIDTH = 2, HOME_MARGIN = 32 };

void home_edge(lv_obj_t *obj, lv_border_side_t side);

typedef enum {
    HOME_AGENDA, HOME_BOOK, HOME_SETTINGS,
    HOME_WAVE, HOME_PLAY, HOME_CARD, HOME_TIMER, HOME_ARROW, HOME_BACK, HOME_KEY
} home_glyph_t;

lv_obj_t *home_glyph(lv_obj_t *parent, home_glyph_t glyph, int size);
