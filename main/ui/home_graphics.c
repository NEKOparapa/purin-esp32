#include "home_graphics.h"

// Icons share a 32-unit grid. Negative points lift the pen between strokes.
typedef struct { int8_t x, y; } point_t;
#define PEN_UP {-1, -1}
#define END {-2, -2}
static const point_t *const paths[] = {
    [HOME_MARK] = (const point_t[]){{2,24},{9,7},{13,7},{7,24},{12,24},{19,7},{23,7},{17,24},{22,24},{29,7},END},
    [HOME_CHAT] = (const point_t[]){{3,5},{29,5},{29,22},{15,22},{8,28},{8,22},{3,22},{3,5},PEN_UP,{9,12},{23,12},END},
    [HOME_AGENDA] = (const point_t[]){{4,8},{28,8},{28,28},{4,28},{4,8},PEN_UP,{4,14},{28,14},PEN_UP,{10,4},{10,10},PEN_UP,{22,4},{22,10},PEN_UP,{9,20},{15,20},END},
    [HOME_BOOK] = (const point_t[]){{2,6},{9,6},{16,10},{23,6},{30,6},{30,24},{23,24},{16,28},{9,24},{2,24},{2,6},PEN_UP,{16,10},{16,28},END},
    [HOME_SETTINGS] = (const point_t[]){{3,9},{10,9},PEN_UP,{17,9},{29,9},PEN_UP,{10,6},{17,6},{17,12},{10,12},{10,6},PEN_UP,{3,23},{18,23},PEN_UP,{25,23},{29,23},PEN_UP,{18,20},{25,20},{25,26},{18,26},{18,20},END},
    [HOME_WAVE] = (const point_t[]){{2,16},{6,16},{9,8},{14,25},{19,5},{23,21},{26,16},{30,16},END},
    [HOME_PLAY] = (const point_t[]){{9,4},{27,16},{9,28},{9,4},END},
    [HOME_CARD] = (const point_t[]){{3,5},{29,5},{29,26},{3,26},{3,5},PEN_UP,{8,12},{24,12},PEN_UP,{8,18},{18,18},END},
    [HOME_TIMER] = (const point_t[]){{10,7},{22,7},{28,13},{28,22},{21,29},{11,29},{4,22},{4,13},{10,7},PEN_UP,{12,2},{20,2},PEN_UP,{16,2},{16,7},PEN_UP,{16,12},{16,18},{21,18},END},
    [HOME_ARROW] = (const point_t[]){{3,16},{28,16},PEN_UP,{19,7},{28,16},{19,25},END},
    [HOME_BACK] = (const point_t[]){{28,16},{4,16},PEN_UP,{13,7},{4,16},{13,25},END},
};

void home_edge(lv_obj_t *obj, lv_border_side_t side)
{
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, HOME_RULE_WIDTH, 0);
    lv_obj_set_style_border_side(obj, side, 0);
}

static void draw_glyph(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const point_t *p = lv_event_get_user_data(event);
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    int side = lv_area_get_width(&bounds);
    lv_draw_line_dsc_t pen;
    lv_draw_line_dsc_init(&pen);
    pen.color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    pen.width = HOME_RULE_WIDTH;
    lv_layer_t *layer = lv_event_get_layer(event);
    for (; p[0].x != -2; ++p) {
        if (p[0].x < 0 || p[1].x < 0) continue;
        pen.p1.x = bounds.x1 + p[0].x * side / 32;
        pen.p1.y = bounds.y1 + p[0].y * side / 32;
        pen.p2.x = bounds.x1 + p[1].x * side / 32;
        pen.p2.y = bounds.y1 + p[1].y * side / 32;
        lv_draw_line(layer, &pen);
    }
}

lv_obj_t *home_glyph(lv_obj_t *parent, home_glyph_t glyph, int size)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, size, size);
    lv_obj_add_event_cb(obj, draw_glyph, LV_EVENT_DRAW_MAIN, (void *)paths[glyph]);
    return obj;
}
