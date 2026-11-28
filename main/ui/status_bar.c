#include "status_bar.h"
#include "eink_ui.h"
#include "terminal_network.h"
#include <time.h>

// Each 32 x 24 design glyph has an eight-byte I1 palette followed by its pixels.
// WAVE_RECORDING uses Lucide's Audio Lines glyph (see art/LUCIDE-LICENSE.txt).
extern const uint8_t status_icons[] asm("_binary_status_icons_i1_start");
enum {
    WAVE_RECORDING, WAVE_STARTING, WAVE_SAVING = 3, WAVE_ERROR,
    NET_OFFLINE, NET_CONNECTING, NET_WIFI, NET_4G,
    BAT_UNKNOWN, BAT_LOW, BAT_ONE, BAT_TWO, BAT_THREE, BAT_FOUR, ICON_COUNT,
    ICON_BYTES = 8 + 4 * 24
};

typedef struct {
    lv_obj_t *clock, *recording, *network, *battery;
} bar_icons_t;

static bar_icons_t icons;
static lv_image_dsc_t images[ICON_COUNT];
static unsigned wave = ICON_COUNT, network = NET_OFFLINE, battery = BAT_UNKNOWN;
static time_t displayed_minute = -1;

static void update_clock(void)
{
    time_t now = time(NULL);
    time_t minute = now / 60;
    if (!icons.clock || minute == displayed_minute) return;
    displayed_minute = minute;

    struct tm local;
    char text[6];
    bool valid = localtime_r(&now, &local) && local.tm_year >= 120 && local.tm_year <= 199 &&
                 strftime(text, sizeof(text), "%H:%M", &local) == 5;
    lv_label_set_text(icons.clock, valid ? text : "--:--");
}

static void set_icon(lv_obj_t *obj, unsigned icon)
{
    if (lv_image_get_src(obj) != &images[icon]) lv_image_set_src(obj, &images[icon]);
}

static void update_bar(bar_icons_t *bar)
{
    if (!bar->recording) return;
    if (wave == ICON_COUNT) {
        lv_obj_add_flag(bar->recording, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_icon(bar->recording, wave);
        lv_obj_remove_flag(bar->recording, LV_OBJ_FLAG_HIDDEN);
    }
    set_icon(bar->network, network);
    set_icon(bar->battery, battery);
}

static lv_obj_t *icon_at(lv_obj_t *parent, lv_align_t align, int x)
{
    lv_obj_t *icon = lv_image_create(parent);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(icon, align, x, 0);
    return icon;
}

lv_obj_t *status_bar_create(lv_obj_t *parent)
{
    if (!images[0].data) {
        for (unsigned i = 0; i < ICON_COUNT; ++i) {
            images[i] = (lv_image_dsc_t){
                .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I1,
                           .w = 32, .h = 24, .stride = 4},
                .data_size = ICON_BYTES, .data = status_icons + i * ICON_BYTES,
            };
        }
    }

    lv_obj_t *row = lv_obj_create(parent);
    eink_style_clean_box(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), 54);
    icons.clock = eink_label(row, "--:--", &lv_font_montserrat_20, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(icons.clock, LV_ALIGN_LEFT_MID, 24, 0);
    displayed_minute = -1;
    update_clock();
    icons.recording = icon_at(row, LV_ALIGN_LEFT_MID, 96);
    icons.network = icon_at(row, LV_ALIGN_RIGHT_MID, -61);
    icons.battery = icon_at(row, LV_ALIGN_RIGHT_MID, -26);
    update_bar(&icons);
    return row;
}

void status_bar_update(recorder_state_t recorder, int battery_percent)
{
    static const unsigned waves[] = {
        [RECORDER_IDLE] = ICON_COUNT, [RECORDER_STARTING] = WAVE_STARTING,
        [RECORDER_RECORDING] = WAVE_RECORDING, [RECORDER_STOPPING] = WAVE_SAVING,
        [RECORDER_ERROR] = WAVE_ERROR,
    };
    update_clock();
    terminal_network_mode_t mode = terminal_network_mode();
    terminal_link_state_t link = terminal_network_link_state();
    unsigned net = NET_OFFLINE;
    if (mode != TERMINAL_NETWORK_OFF && link == TERMINAL_LINK_CONNECTING) net = NET_CONNECTING;
    if (mode != TERMINAL_NETWORK_OFF && link == TERMINAL_LINK_CONNECTED) {
        net = mode == TERMINAL_NETWORK_WIFI ? NET_WIFI : NET_4G;
    }
    unsigned bat = BAT_UNKNOWN;
    if (battery_percent >= 0 && battery_percent <= 100) {
        if (battery_percent <= 15) bat = BAT_LOW;
        else if (battery_percent <= 25) bat = BAT_ONE;
        else if (battery_percent <= 50) bat = BAT_TWO;
        else if (battery_percent <= 75) bat = BAT_THREE;
        else bat = BAT_FOUR;
    }
    if (wave == waves[recorder] && network == net && battery == bat) return;
    wave = waves[recorder];
    network = net;
    battery = bat;
    update_bar(&icons);
}
