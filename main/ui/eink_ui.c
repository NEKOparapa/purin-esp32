#include "eink_ui.h"
#include "home_graphics.h"

extern const uint8_t ui_sc_start[] asm("_binary_ui_sc_ttf_start");
extern const uint8_t ui_sc_end[] asm("_binary_ui_sc_ttf_end");

static const lv_font_t *heading_font_cache;
static const lv_font_t *small_font_cache;
static const lv_font_t *body_font_cache;

static const lv_font_t *load_sc_font(int px)
{
    lv_font_t *font = lv_tiny_ttf_create_data(ui_sc_start, ui_sc_end - ui_sc_start, px);
    if (font) font->fallback = LV_FONT_DEFAULT;
    return font ? font : LV_FONT_DEFAULT;
}

const lv_font_t *eink_heading_font(void)
{
    if (!heading_font_cache) heading_font_cache = load_sc_font(28);
    return heading_font_cache;
}

const lv_font_t *eink_body_font(void)
{
    if (!body_font_cache) body_font_cache = load_sc_font(22);
    return body_font_cache;
}

const lv_font_t *eink_small_font(void)
{
    if (!small_font_cache) small_font_cache = load_sc_font(18);
    return small_font_cache;
}

void eink_style_clean_box(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void eink_style_screen(lv_obj_t *screen)
{
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_black(), LV_PART_MAIN);
}

void eink_style_card(lv_obj_t *obj, int radius, int border_width)
{
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, border_width, LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}

/* Circular 48x48 Apple Control-Center medallion container */
static lv_obj_t *make_medallion(lv_obj_t *parent, bool on_dark_tile, lv_color_t *out_fg,
                                lv_color_t *out_bg)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_remove_style_all(circle);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(circle, 48, 48);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);

    if (on_dark_tile) {
        // Solid crisp white circle on black card, black glyph inside
        lv_obj_set_style_bg_color(circle, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(circle, 0, LV_PART_MAIN);
    } else {
        // Pure white circle with delicate 1px black ring on white card, black glyph inside
        lv_obj_set_style_bg_color(circle, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_color(circle, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(circle, 1, LV_PART_MAIN);
        lv_obj_set_style_border_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    }

    *out_fg = lv_color_black();
    *out_bg = lv_color_white();

    lv_obj_t *inner = lv_obj_create(circle);
    eink_style_clean_box(inner);
    lv_obj_remove_flag(inner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(inner, 28, 28);
    lv_obj_center(inner);
    return inner;
}

static lv_obj_t *make_shape(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                            lv_color_t fill, lv_color_t border, int border_w)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_obj_set_style_radius(s, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, fill, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    if (border_w > 0) {
        lv_obj_set_style_border_color(s, border, LV_PART_MAIN);
        lv_obj_set_style_border_width(s, border_w, LV_PART_MAIN);
        lv_obj_set_style_border_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    }
    return s;
}

lv_obj_t *eink_icon_calendar(lv_obj_t *parent, bool on_dark_tile)
{
    lv_color_t fg, bg;
    lv_obj_t *box = make_medallion(parent, on_dark_tile, &fg, &bg);
    make_shape(box, 8, 2, 2, 5, 1, fg, fg, 0);
    make_shape(box, 18, 2, 2, 5, 1, fg, fg, 0);
    make_shape(box, 3, 5, 22, 20, 6, bg, fg, 2);
    make_shape(box, 5, 7, 18, 5, 2, fg, fg, 0);
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) {
            make_shape(box, 7 + c * 5, 15 + r * 5, 3, 3, LV_RADIUS_CIRCLE, fg, fg, 0);
        }
    }
    return lv_obj_get_parent(box);
}

lv_obj_t *eink_icon_reading(lv_obj_t *parent, bool on_dark_tile)
{
    lv_color_t fg, bg;
    lv_obj_t *box = make_medallion(parent, on_dark_tile, &fg, &bg);
    make_shape(box, 4, 2, 20, 24, 5, bg, fg, 2);
    make_shape(box, 8, 8, 12, 2, 1, fg, fg, 0);
    make_shape(box, 8, 13, 12, 2, 1, fg, fg, 0);
    make_shape(box, 8, 18, 8, 2, 1, fg, fg, 0);
    return lv_obj_get_parent(box);
}

lv_obj_t *eink_icon_settings(lv_obj_t *parent, bool on_dark_tile)
{
    lv_color_t fg, bg;
    lv_obj_t *box = make_medallion(parent, on_dark_tile, &fg, &bg);
    make_shape(box, 3, 9, 22, 2, 1, fg, fg, 0);
    make_shape(box, 15, 6, 8, 8, LV_RADIUS_CIRCLE, bg, fg, 2);
    make_shape(box, 3, 19, 22, 2, 1, fg, fg, 0);
    make_shape(box, 6, 16, 8, 8, LV_RADIUS_CIRCLE, bg, fg, 2);
    return lv_obj_get_parent(box);
}

lv_obj_t *eink_brand_mark(lv_obj_t *parent, int size)
{
    lv_obj_t *mark = lv_obj_create(parent);
    lv_obj_remove_style_all(mark);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mark, size, size);
    lv_obj_set_style_bg_color(mark, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_flex_flow(mark, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mark, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mark, size >= 32 ? 3 : 2, LV_PART_MAIN);

    static const int ratios[5] = {24, 42, 60, 36, 26};
    int bar_w = size >= 36 ? 3 : 2;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *bar = lv_obj_create(mark);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        int bar_h = (size * ratios[i]) / 100;
        if (bar_h < 4) bar_h = 4;
        lv_obj_set_size(bar, bar_w, bar_h);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    }
    return mark;
}

lv_obj_t *eink_divider(lv_obj_t *parent, int width)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(line, width, 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    return line;
}

lv_obj_t *eink_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                     lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_remove_style_all(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN);
    if (font) lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, align, LV_PART_MAIN);
    lv_label_set_text(lbl, text ? text : "");
    return lbl;
}

lv_obj_t *eink_pill_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           int height, bool primary, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(btn, height);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN);

    if (primary) {
        lv_obj_set_style_bg_color(btn, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_PART_MAIN);

        lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_PRESSED);

        lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_DISABLED);
        lv_obj_set_style_border_width(btn, 1, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_DISABLED);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_black(), LV_PART_MAIN);

        lv_obj_set_style_bg_color(btn, lv_color_black(), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_PRESSED);

        lv_obj_set_style_bg_color(btn, lv_color_black(), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);

        lv_obj_set_style_border_width(btn, 1, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_DISABLED);
    }

    if (font) lv_obj_set_style_text_font(btn, font, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_center(lbl);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

lv_obj_t *eink_outline_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              int height, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    eink_style_clean_box(button);
    eink_style_card(button, EINK_RADIUS_CONTROL, EINK_BORDER_WIDTH);
    lv_obj_set_height(button, height);
    lv_obj_set_style_pad_hor(button, 16, LV_PART_MAIN);
    // Keep antialiased text black on paper in every state for the I1 renderer.
    lv_obj_set_style_text_decor(button, LV_TEXT_DECOR_UNDERLINE, LV_STATE_PRESSED);
    lv_obj_t *label = eink_label(button, text, font, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    return button;
}

lv_obj_t *eink_refresh_button(lv_obj_t *parent, lv_event_cb_t on_refresh)
{
    lv_obj_t *btn = lv_button_create(parent);
    eink_style_clean_box(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, 48, 48);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *icon = eink_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(icon);
    if (on_refresh) lv_obj_add_event_cb(btn, on_refresh, LV_EVENT_CLICKED, NULL);
    return btn;
}

lv_obj_t *eink_back_button(lv_obj_t *parent, lv_event_cb_t on_back)
{
    lv_obj_t *back = lv_button_create(parent);
    eink_style_clean_box(back);
    lv_obj_set_size(back, 52, 56);
    lv_obj_set_style_bg_color(back, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(back, lv_color_white(), LV_STATE_PRESSED);

    lv_obj_t *arrow = home_glyph(back, HOME_BACK, 28);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    return back;
}

lv_obj_t *eink_top_bar_font(lv_obj_t *screen, const char *title, const lv_font_t *title_font,
                            lv_event_cb_t on_back, lv_obj_t **right_slot)
{
    lv_obj_t *bar = lv_obj_create(screen);
    eink_style_clean_box(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, EINK_TOP_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, LV_PART_MAIN);

    if (on_back) eink_back_button(bar, on_back);

    lv_obj_t *heading = eink_label(bar, title, title_font ? title_font : &lv_font_montserrat_28,
                                   LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(heading, 1);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_CLIP);

    if (right_slot) {
        lv_obj_t *slot = lv_obj_create(bar);
        eink_style_clean_box(slot);
        lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(slot, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(slot, 8, LV_PART_MAIN);
        *right_slot = slot;
    }

    return bar;
}

lv_obj_t *eink_top_bar(lv_obj_t *screen, const char *title, lv_event_cb_t on_back,
                       lv_obj_t **right_slot)
{
    return eink_top_bar_font(screen, title, &lv_font_montserrat_28, on_back, right_slot);
}

static void textarea_frame_clicked(lv_event_t *event)
{
    if (lv_event_get_target(event) == lv_event_get_current_target(event)) {
        lv_obj_send_event(lv_event_get_user_data(event), LV_EVENT_CLICKED, NULL);
    }
}

lv_obj_t *eink_textarea(lv_obj_t *parent, const lv_font_t *font, int height, int radius,
                       lv_obj_t **input_out)
{
    // Keep the outline outside the scrolling text so it stays clear when focused.
    const int border_width = 2;
    lv_obj_t *frame = lv_obj_create(parent);
    eink_style_clean_box(frame);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    eink_style_card(frame, radius, border_width);
    lv_obj_set_height(frame, height);
    lv_obj_set_style_pad_hor(frame, 16, LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(frame);
    lv_obj_remove_style_all(ta);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_size(ta, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_color(ta, lv_color_black(), LV_PART_MAIN);
    if (font) lv_obj_set_style_text_font(ta, font, LV_PART_MAIN);
    int32_t line_height = lv_font_get_line_height(lv_obj_get_style_text_font(ta, LV_PART_MAIN));
    int32_t pad_y = (height - line_height) / 2 - border_width;
    lv_obj_set_style_pad_hor(ta, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ta, LV_MAX(0, pad_y), LV_PART_MAIN);

    lv_obj_set_style_border_color(ta, lv_color_black(), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(frame, textarea_frame_clicked, LV_EVENT_CLICKED, ta);
    *input_out = ta;
    return frame;
}

void eink_style_keyboard(lv_obj_t *panel)
{
    // LVGL marks function keys as checked. Give them the same ink as letter keys.
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_color(panel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(panel, 5, LV_PART_MAIN);

    lv_obj_set_style_border_color(panel, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(panel, 2, LV_PART_ITEMS);
    lv_obj_set_style_radius(panel, 8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(panel, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(panel, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_border_width(panel, 4, LV_PART_ITEMS | LV_STATE_PRESSED);
}
