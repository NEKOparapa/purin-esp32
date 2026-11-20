#pragma once

#include <stdbool.h>
#include "lvgl.h"

/*
 * Shared monochrome 1-bit E-Ink design primitives (480x800 portrait).
 * Apple / Braun inspired: generous paper whitespace, crisp 1-2px geometry,
 * pill capsules, clean black/white inversion, zero grayscale dithering or animations.
 */

enum {
    EINK_PAD_X = 24,
    EINK_TOP_BAR_H = 68,
    EINK_RADIUS_CARD = 20,
    EINK_RADIUS_CONTROL = 16,
    EINK_BORDER_WIDTH = 2,
};

const lv_font_t *eink_heading_font(void);
const lv_font_t *eink_body_font(void);
const lv_font_t *eink_small_font(void);

void eink_style_screen(lv_obj_t *screen);
void eink_style_clean_box(lv_obj_t *obj);
void eink_style_card(lv_obj_t *obj, int radius, int border_width);

lv_obj_t *eink_brand_mark(lv_obj_t *parent, int size);
lv_obj_t *eink_divider(lv_obj_t *parent, int width);

/* Bespoke 1-bit circular icon medallions (46x46) for the 2x2 Home Bento tiles */
lv_obj_t *eink_icon_chat(lv_obj_t *parent, bool on_dark_tile);
lv_obj_t *eink_icon_calendar(lv_obj_t *parent, bool on_dark_tile);
lv_obj_t *eink_icon_reading(lv_obj_t *parent, bool on_dark_tile);
lv_obj_t *eink_icon_settings(lv_obj_t *parent, bool on_dark_tile);

lv_obj_t *eink_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                     lv_text_align_t align);

lv_obj_t *eink_pill_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           int height, bool primary, lv_event_cb_t cb, void *user_data);
lv_obj_t *eink_outline_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              int height, lv_event_cb_t cb, void *user_data);

lv_obj_t *eink_badge(lv_obj_t *parent, const char *text, const lv_font_t *font, bool filled);

lv_obj_t *eink_back_button(lv_obj_t *parent, lv_event_cb_t on_back);
lv_obj_t *eink_top_bar(lv_obj_t *screen, const char *title, lv_event_cb_t on_back,
                       lv_obj_t **right_slot);

lv_obj_t *eink_top_bar_font(lv_obj_t *screen, const char *title, const lv_font_t *title_font,
                            lv_event_cb_t on_back, lv_obj_t **right_slot);

lv_obj_t *eink_textarea(lv_obj_t *parent, const lv_font_t *font, int height, int radius,
                       lv_obj_t **input_out);
void eink_style_keyboard(lv_obj_t *kb);
void eink_style_candidates(lv_obj_t *cand, const lv_font_t *font);
