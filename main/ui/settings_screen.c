#include "settings_screen.h"
#include "board.h"
#include "device_apps_screen.h"
#include "eink_ui.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "lvgl_port.h"
#include "reading_progress.h"
#include "recorder.h"
#include "terminal_network.h"
#include "terminal_pairing.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

enum { WIFI_SCAN_LIMIT = 16 };
typedef enum {
    SET_NETWORK_MODE, SET_WIFI_SCAN, SET_WIFI_JOIN, SET_READ_STORAGE, SET_PAIR_DEVICE
} set_action_t;

typedef struct {
    set_action_t action;
    terminal_network_mode_t mode;
    terminal_wifi_ap_t ap;
    char *password;
    char *pair_link;
} set_cmd_t;

typedef struct {
    set_action_t action;
    bool ok;
    char error[128];
    size_t ap_count;
    terminal_wifi_ap_t aps[WIFI_SCAN_LIMIT];
    terminal_wifi_join_result_t joined;
    uint32_t flash_bytes;
    uint64_t sd_total_bytes;
    uint64_t sd_free_bytes;
    esp_err_t sd_error;
} set_res_t;

static lv_obj_t *home_ref;
static lv_obj_t *index_scr;
static lv_obj_t *connection_scr;
static lv_obj_t *pair_scr;
static lv_obj_t *pair_address;
static lv_obj_t *pair_keyboard;
static lv_obj_t *pair_button;
static lv_obj_t *pair_status;
static lv_obj_t *network_scr;
static lv_obj_t *wifi_scan_scr;
static lv_obj_t *wifi_password_scr;
static lv_obj_t *connectors_scr;
static lv_obj_t *appearance_scr;
static lv_obj_t *collection_scr;
static lv_obj_t *storage_scr;

static lv_obj_t *idx_network_val;
static lv_obj_t *idx_rec_val;
static lv_obj_t *network_status_val;
static lv_obj_t *wifi_name_val;
static lv_obj_t *wifi_scan_button;
static lv_obj_t *wifi_scan_status;
static lv_obj_t *wifi_scan_list;
static lv_obj_t *wifi_password;
static lv_obj_t *wifi_password_ssid;
static lv_obj_t *wifi_password_status;
static lv_obj_t *wifi_keyboard;
static lv_obj_t *wifi_join_button;
static lv_obj_t *wifi_show_button;
static terminal_wifi_ap_t wifi_aps[WIFI_SCAN_LIMIT];
static terminal_wifi_ap_t wifi_selected_ap;
static lv_obj_t *network_options[TERMINAL_NETWORK_OFF + 1];
static lv_obj_t *rec_status_val;
static lv_obj_t *recording_manual_button;
static lv_obj_t *recording_auto_button;
static lv_obj_t *recording_upload_note;
static lv_obj_t *reading_upload_note;
static lv_obj_t *flash_capacity_val;
static lv_obj_t *sd_status_val;
static lv_obj_t *sd_total_val;
static lv_obj_t *sd_used_val;
static lv_obj_t *sd_free_val;

static QueueHandle_t cmds, results;
static bool busy;
static bool network_pending;
static bool network_save_failed;
static bool recording_auto_upload;
static bool reading_progress_upload;

static const char recording_upload_key[] = "audio_auto";
static const char recording_upload_hint[] =
    "自动上传待实现。\n长按 BOOT 开始或停止录音。录音中短按 BOOT 选择时长，再次短按确认上传。";
static const char reading_upload_hint[] = "退出阅读时上报有变化的进度。离线时会稍后重试。";

static void render_wifi_scan(const set_res_t *res);
static void render_wifi_join(const set_res_t *res);
static void ssid_label(char out[TERMINAL_WIFI_SSID_SIZE], const char *ssid);

static const char *recorder_label_zh(void)
{
    recorder_status_t st = recorder_get_status();
    switch (st.state) {
    case RECORDER_RECORDING: return "录音中";
    case RECORDER_STARTING: return "启动中";
    case RECORDER_STOPPING: return "保存中";
    case RECORDER_ERROR: return "异常";
    default: return "就绪";
    }
}

static void free_wifi_password(char *password)
{
    if (!password) return;
    mbedtls_platform_zeroize(password, strlen(password));
    free(password);
}

static void settings_worker(void *unused)
{
    (void)unused;
    set_cmd_t cmd;
    for (;;) {
        xQueueReceive(cmds, &cmd, portMAX_DELAY);
        set_res_t res = {.action = cmd.action};
        if (cmd.action == SET_NETWORK_MODE) {
            esp_err_t error = terminal_network_select(cmd.mode);
            res.ok = error == ESP_OK || terminal_network_mode() == cmd.mode;
        } else if (cmd.action == SET_WIFI_SCAN) {
            esp_err_t error = terminal_wifi_scan(res.aps, WIFI_SCAN_LIMIT, &res.ap_count);
            res.ok = error == ESP_OK;
            if (error == ESP_ERR_NO_MEM) {
                strlcpy(res.error, "扫描所需内存不足，请点刷新重试。", sizeof(res.error));
            }
        } else if (cmd.action == SET_WIFI_JOIN) {
            res.joined = terminal_wifi_join(&cmd.ap, cmd.password ? cmd.password : "");
            free_wifi_password(cmd.password);
        } else if (cmd.action == SET_READ_STORAGE) {
            res.ok = esp_flash_get_physical_size(NULL, &res.flash_bytes) == ESP_OK;
            res.sd_error = board_sdcard_read_usage(&res.sd_total_bytes, &res.sd_free_bytes);
        } else if (cmd.action == SET_PAIR_DEVICE) {
            res.ok = terminal_pair_device(cmd.pair_link, res.error, sizeof(res.error));
            mbedtls_platform_zeroize(cmd.pair_link, strlen(cmd.pair_link));
            free(cmd.pair_link);
        }
        xQueueSend(results, &res, portMAX_DELAY);
    }
}

static bool queue_setting(set_cmd_t cmd)
{
    if (!cmds || busy) return false;
    busy = xQueueSend(cmds, &cmd, 0) == pdTRUE;
    return busy;
}

static void add_inset_divider(lv_obj_t *parent)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    eink_style_clean_box(wrap);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, 1);
    lv_obj_set_style_pad_hor(wrap, 20, LV_PART_MAIN);
    eink_divider(wrap, LV_PCT(100));
}

static lv_obj_t *make_settings_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    eink_style_clean_box(card);
    eink_style_card(card, EINK_RADIUS_CARD, EINK_BORDER_WIDTH);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static lv_obj_t *make_info_row(lv_obj_t *parent, const char *key, const char *val,
                               lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    eink_style_clean_box(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 68, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);

    eink_label(row, key, eink_body_font(), LV_TEXT_ALIGN_LEFT);
    if (val || val_out) {
        lv_obj_t *v = eink_label(row, val ? val : "", eink_small_font(), LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_flex_grow(v, 1);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
        if (val_out) *val_out = v;
    }
    return row;
}

static lv_obj_t *make_collection_note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = eink_label(parent, text, eink_small_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_hor(label, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 12, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 4, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    return label;
}

static lv_obj_t *make_category_row(lv_obj_t *parent, const char *title, const char *right_text,
                                   lv_obj_t **val_out, lv_event_cb_t on_click)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 80);
    lv_obj_set_style_radius(row, EINK_RADIUS_CONTROL, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, EINK_BORDER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(row, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 22, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_font(lbl, eink_body_font(), LV_PART_MAIN);
    lv_label_set_text(lbl, title);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, eink_small_font(), LV_PART_MAIN);
    lv_label_set_text(val, right_text ? right_text : "");
    if (val_out) *val_out = val;

    lv_obj_t *chev = lv_label_create(row);
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);

    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

static void back_to_home(lv_event_t *event)
{
    (void)event;
    if (home_ref) lv_screen_load(home_ref);
}

static void back_to_index(lv_event_t *event)
{
    (void)event;
    settings_screen_refresh_state();
    if (index_scr) lv_screen_load(index_scr);
}

static void trigger_full_refresh(lv_event_t *event)
{
    (void)event;
    lvgl_port_request_full_refresh();
}

static void set_capacity(lv_obj_t *label, uint64_t bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    unsigned unit = 0;
    uint64_t scale = 1;
    while (unit + 1 < sizeof(units) / sizeof(units[0]) && bytes / scale >= 1024) {
        ++unit;
        scale *= 1024;
    }
    const uint64_t whole = bytes / scale;
    const unsigned tenth = (unsigned)((bytes % scale) * 10 / scale);
    char text[32];
    if (unit == 0) snprintf(text, sizeof(text), "%" PRIu64 " B", whole);
    else snprintf(text, sizeof(text), "%" PRIu64 ".%u %s", whole, tenth, units[unit]);
    lv_label_set_text(label, text);
}

static void render_storage_result(const set_res_t *res)
{
    if (res->ok) set_capacity(flash_capacity_val, res->flash_bytes);
    else lv_label_set_text(flash_capacity_val, "无法读取");

    if (res->sd_error == ESP_OK) {
        lv_label_set_text(sd_status_val, "可用");
        set_capacity(sd_total_val, res->sd_total_bytes);
        set_capacity(sd_used_val, res->sd_total_bytes - res->sd_free_bytes);
        set_capacity(sd_free_val, res->sd_free_bytes);
    } else {
        lv_label_set_text(sd_status_val, res->sd_error == ESP_ERR_INVALID_STATE
                            ? "使用中，请稍后刷新" : "未检测到或无法读取");
        lv_label_set_text(sd_total_val, "-");
        lv_label_set_text(sd_used_val, "-");
        lv_label_set_text(sd_free_val, "-");
    }
}

static void refresh_network_state(void)
{
    static const char *const mode_names[] = {
        [TERMINAL_NETWORK_WIFI] = "Wi-Fi",
        [TERMINAL_NETWORK_CELLULAR] = "移动网络",
        [TERMINAL_NETWORK_OFF] = "已关闭",
    };
    terminal_network_mode_t mode = terminal_network_mode();
    if (idx_network_val && strcmp(lv_label_get_text(idx_network_val), mode_names[mode])) {
        lv_label_set_text(idx_network_val, mode_names[mode]);
    }
    if (!network_status_val) return;

    char ssid[TERMINAL_WIFI_SSID_SIZE];
    bool configured = terminal_wifi_saved_ssid(ssid);
    char name[TERMINAL_WIFI_SSID_SIZE];
    if (configured) ssid_label(name, ssid);
    const char *saved_name = configured ? name : "未配置";
    if (strcmp(lv_label_get_text(wifi_name_val), saved_name)) {
        lv_label_set_text(wifi_name_val, saved_name);
    }
    const char *status;
    if (network_pending) status = "正在应用…";
    else if (network_save_failed) status = "无法保存设置";
    else if (mode == TERMINAL_NETWORK_OFF) status = "已关闭";
    else {
        switch (terminal_network_link_state()) {
        case TERMINAL_LINK_CONNECTING: status = "正在连接…"; break;
        case TERMINAL_LINK_CONNECTED: status = "已连接"; break;
        case TERMINAL_LINK_NO_SSID: status = "未配置 Wi-Fi"; break;
        case TERMINAL_LINK_NO_SIM: status = "未检测到 SIM 卡"; break;
        case TERMINAL_LINK_FAILED: status = "连接失败，点选重试"; break;
        default:
            status = mode == TERMINAL_NETWORK_WIFI && !configured
                ? "未配置 Wi-Fi" : "待连接";
            break;
        }
    }
    if (strcmp(lv_label_get_text(network_status_val), status)) {
        lv_label_set_text(network_status_val, status);
    }
    for (unsigned i = 0; i < sizeof(network_options) / sizeof(network_options[0]); ++i) {
        lv_obj_t *btn = network_options[i];
        lv_obj_set_flag(lv_obj_get_child(btn, 0), LV_OBJ_FLAG_HIDDEN, i != (unsigned)mode);
        if (busy || !cmds) lv_obj_add_state(btn, LV_STATE_DISABLED);
        else lv_obj_remove_state(btn, LV_STATE_DISABLED);
    }
    if (mode != TERMINAL_NETWORK_WIFI || busy || !cmds) lv_obj_add_state(wifi_scan_button, LV_STATE_DISABLED);
    else lv_obj_remove_state(wifi_scan_button, LV_STATE_DISABLED);
}

static bool can_restart(void)
{
    recorder_state_t state = recorder_get_status().state;
    return state == RECORDER_IDLE || state == RECORDER_ERROR;
}

static void restart_after_pairing(lv_timer_t *timer)
{
    (void)timer;
    if (can_restart()) esp_restart();
    lv_label_set_text(pair_status, "已保存，录音结束后自动重启。");
}

static void poll_results(lv_timer_t *timer)
{
    (void)timer;
    set_res_t res;
    if (results && xQueueReceive(results, &res, 0) == pdTRUE) {
        busy = false;
        if (res.action == SET_NETWORK_MODE) {
            network_pending = false;
            network_save_failed = !res.ok;
        } else if (res.action == SET_WIFI_SCAN) {
            render_wifi_scan(&res);
        } else if (res.action == SET_WIFI_JOIN) {
            render_wifi_join(&res);
        } else if (res.action == SET_READ_STORAGE) {
            render_storage_result(&res);
        } else if (res.action == SET_PAIR_DEVICE) {
            lv_label_set_text(pair_status, res.ok ? "已配对，正在重启连接…" : res.error);
            if (res.ok) {
                lv_textarea_set_text(pair_address, "");
                lv_timer_create(restart_after_pairing, 1000, NULL);
            } else {
                lv_obj_remove_state(pair_button, LV_STATE_DISABLED);
            }
        }
    }
    refresh_network_state();
}

static void ensure_worker(void)
{
    if (cmds) return;
    cmds = xQueueCreate(1, sizeof(set_cmd_t));
    results = xQueueCreate(1, sizeof(set_res_t));
    if (!cmds || !results ||
        xTaskCreate(settings_worker, "settings_io", 10240, NULL, 1, NULL) != pdPASS) {
        if (cmds) vQueueDelete(cmds);
        if (results) vQueueDelete(results);
        cmds = results = NULL;
        return;
    }
    lv_timer_create(poll_results, 250, NULL);
}

static lv_obj_t *create_subpage_shell(const char *title, lv_obj_t **body_out,
                                     lv_event_cb_t on_back, lv_event_cb_t on_refresh)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    eink_style_screen(scr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *right_slot = NULL;
    eink_top_bar_font(scr, title, eink_heading_font(), on_back,
                      on_refresh ? &right_slot : NULL);
    if (on_refresh && right_slot) {
        lv_obj_t *btn =
            eink_outline_button(right_slot, "刷新", eink_small_font(), 40, on_refresh, NULL);
        lv_obj_set_style_pad_hor(btn, 14, LV_PART_MAIN);
    }

    lv_obj_t *body = lv_obj_create(scr);
    eink_style_clean_box(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(body, EINK_PAD_X, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 18, LV_PART_MAIN);

    if (body_out) *body_out = body;
    return scr;
}

static void ssid_label(char out[TERMINAL_WIFI_SSID_SIZE], const char *ssid)
{
    strlcpy(out, ssid, TERMINAL_WIFI_SSID_SIZE);
    for (char *c = out; *c; ++c) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = '?';
    }
}

static void back_to_network(lv_event_t *event)
{
    (void)event;
    refresh_network_state();
    lv_screen_load(network_scr);
}

static void back_to_wifi_scan(lv_event_t *event)
{
    (void)event;
    lv_textarea_set_text(wifi_password, "");
    lv_screen_load(wifi_scan_scr);
}

static void set_wifi_scan_status(const char *message)
{
    lv_label_set_text(wifi_scan_status, message);
    if (*message) lv_obj_remove_flag(wifi_scan_status, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wifi_scan_status, LV_OBJ_FLAG_HIDDEN);
}

static void scan_wifi_clicked(lv_event_t *event)
{
    (void)event;
    if (busy) return;
    lv_obj_clean(wifi_scan_list);
    bool started = queue_setting((set_cmd_t){.action = SET_WIFI_SCAN});
    set_wifi_scan_status(started ? "正在扫描附近的 Wi-Fi…" : "无法开始扫描，请重试。");
}

static void open_wifi_scan(lv_event_t *event)
{
    (void)event;
    if (!wifi_scan_scr) {
        lv_obj_t *body;
        wifi_scan_scr = create_subpage_shell("附近的 Wi-Fi", &body, back_to_network, scan_wifi_clicked);
        wifi_scan_status = eink_label(body, "", eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(wifi_scan_status, LV_PCT(100));
        wifi_scan_list = lv_obj_create(body);
        eink_style_clean_box(wifi_scan_list);
        lv_obj_remove_flag(wifi_scan_list, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(wifi_scan_list, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    }
    lv_screen_load(wifi_scan_scr);
    scan_wifi_clicked(NULL);
}

static bool request_wifi_join(const terminal_wifi_ap_t *ap, const char *password)
{
    char *copy = password[0] ? strdup(password) : NULL;
    if (password[0] && !copy) return false;
    if (!queue_setting((set_cmd_t){.action = SET_WIFI_JOIN, .ap = *ap, .password = copy})) {
        free_wifi_password(copy);
        return false;
    }
    return true;
}

static void enable_wifi_password_controls(bool enabled)
{
    lv_obj_t *controls[] = {wifi_password, wifi_keyboard, wifi_join_button, wifi_show_button};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (enabled) lv_obj_remove_state(controls[i], LV_STATE_DISABLED);
        else lv_obj_add_state(controls[i], LV_STATE_DISABLED);
    }
}

static void join_wifi_clicked(lv_event_t *event)
{
    (void)event;
    if (busy) return;
    const char *password = lv_textarea_get_text(wifi_password);
    if (strlen(password) < 8) {
        lv_label_set_text(wifi_password_status, "密码至少需要 8 个字符。");
        return;
    }
    if (request_wifi_join(&wifi_selected_ap, password)) {
        lv_textarea_set_password_mode(wifi_password, true);
        lv_label_set_text(lv_obj_get_child(wifi_show_button, 0), "显示密码");
        enable_wifi_password_controls(false);
        lv_label_set_text(wifi_password_status, "正在连接…");
    } else {
        lv_label_set_text(wifi_password_status, "无法开始连接，请重试。");
    }
}

static void toggle_wifi_password(lv_event_t *event)
{
    bool masked = lv_textarea_get_password_mode(wifi_password);
    lv_textarea_set_password_mode(wifi_password, !masked);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(event), 0), masked ? "隐藏密码" : "显示密码");
}

static void wifi_password_changed(lv_event_t *event)
{
    (void)event;
    if (!busy && *lv_label_get_text(wifi_password_status)) lv_label_set_text(wifi_password_status, "");
}

static void wifi_symbol_keys(lv_event_t *event)
{
    lv_obj_t *kb = lv_event_get_target_obj(event);
    const char *text = lv_keyboard_get_button_text(kb, lv_keyboard_get_selected_button(kb));
    if (text && !strcmp(text, "1#")) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
        lv_event_stop_processing(event);
    }
}

static void open_wifi_password(void)
{
    if (!wifi_password_scr) {
        lv_obj_t *body;
        wifi_password_scr = create_subpage_shell("连接 Wi-Fi", &body, back_to_wifi_scan, NULL);
        wifi_password_ssid = eink_label(body, "", eink_heading_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(wifi_password_ssid, LV_PCT(100));
        lv_obj_t *frame = eink_textarea(body, eink_body_font(), 54, EINK_RADIUS_CONTROL, &wifi_password);
        lv_obj_set_width(frame, LV_PCT(100));
        lv_textarea_set_placeholder_text(wifi_password, "输入 Wi-Fi 密码");
        lv_textarea_set_max_length(wifi_password, TERMINAL_WIFI_PASSWORD_SIZE - 1);
        lv_textarea_set_password_show_time(wifi_password, 0);

        lv_obj_t *actions = lv_obj_create(body);
        eink_style_clean_box(actions);
        lv_obj_remove_flag(actions, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(actions, LV_PCT(100), 48);
        lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        wifi_show_button = eink_outline_button(actions, "显示密码", eink_body_font(), 48,
                                               toggle_wifi_password, NULL);
        wifi_join_button = eink_outline_button(actions, "连接", eink_body_font(), 48,
                                               join_wifi_clicked, NULL);
        wifi_password_status = eink_label(body, "", eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(wifi_password_status, LV_PCT(100));
        lv_obj_add_event_cb(wifi_password, wifi_password_changed, LV_EVENT_VALUE_CHANGED, NULL);

        // Use a private keyboard mode so the chat keyboard's layout stays independent.
        static const char *const symbols[] = {
            "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
            "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "-", "\n",
            "+", "=", "[", "]", "{", "}", "\\", "|", ";", ":", "'", "\"", "\n",
            "abc", "`", "~", "<", ">", ",", ".", "/", "?", " ", LV_SYMBOL_OK, "",
        };
        static const lv_buttonmatrix_ctrl_t widths[] = {
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            2, 1, 1, 1, 1, 1, 1, 1, 1, 3, 2,
        };
        wifi_keyboard = lv_keyboard_create(wifi_password_scr);
        eink_style_keyboard(wifi_keyboard);
        lv_obj_set_style_border_width(wifi_keyboard, EINK_BORDER_WIDTH, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_text_decor(wifi_keyboard, LV_TEXT_DECOR_UNDERLINE, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_size(wifi_keyboard, LV_PCT(100), 264);
        lv_keyboard_set_map(wifi_keyboard, LV_KEYBOARD_MODE_USER_1, symbols, widths);
        lv_obj_add_event_cb(wifi_keyboard, wifi_symbol_keys, LV_EVENT_VALUE_CHANGED | LV_EVENT_PREPROCESS, NULL);
        lv_obj_add_event_cb(wifi_keyboard, join_wifi_clicked, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(wifi_keyboard, back_to_wifi_scan, LV_EVENT_CANCEL, NULL);
    }

    char name[TERMINAL_WIFI_SSID_SIZE];
    ssid_label(name, wifi_selected_ap.ssid);
    lv_label_set_text(wifi_password_ssid, name);
    lv_label_set_text(wifi_password_status, "");
    lv_label_set_text(lv_obj_get_child(wifi_show_button, 0), "显示密码");
    lv_textarea_set_text(wifi_password, "");
    lv_textarea_set_password_mode(wifi_password, true);
    enable_wifi_password_controls(true);
    lv_keyboard_set_mode(wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(wifi_keyboard, wifi_password);
    lv_screen_load(wifi_password_scr);
}

static void wifi_ap_clicked(lv_event_t *event)
{
    if (busy) return;
    unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    wifi_selected_ap = wifi_aps[i];
    if (wifi_selected_ap.secured) {
        open_wifi_password();
    } else {
        bool started = request_wifi_join(&wifi_selected_ap, "");
        set_wifi_scan_status(started ? "正在连接…" : "无法开始连接，请重试。");
    }
}

static void render_wifi_scan(const set_res_t *res)
{
    if (!res->ok || !res->ap_count) {
        const char *message = res->ok ? "未找到可连接的 Wi-Fi。" : "扫描失败，请点刷新重试。";
        set_wifi_scan_status(res->error[0] ? res->error : message);
        return;
    }
    set_wifi_scan_status("");
    memcpy(wifi_aps, res->aps, res->ap_count * sizeof(wifi_aps[0]));
    for (size_t i = 0; i < res->ap_count; ++i) {
        char name[TERMINAL_WIFI_SSID_SIZE];
        ssid_label(name, wifi_aps[i].ssid);
        char details[32];
        snprintf(details, sizeof(details), "%s %d dBm", wifi_aps[i].secured ? "加密" : "开放", wifi_aps[i].rssi);
        lv_obj_t *row = make_category_row(wifi_scan_list, name, details, NULL, NULL);
        lv_label_set_long_mode(lv_obj_get_child(row, 0), LV_LABEL_LONG_MODE_DOTS);
        lv_obj_add_event_cb(row, wifi_ap_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        if (i + 1 < res->ap_count) add_inset_divider(wifi_scan_list);
    }
}

static void render_wifi_join(const set_res_t *res)
{
    lv_obj_t *active = lv_screen_active();
    if (wifi_password) enable_wifi_password_controls(true);
    if (res->joined == TERMINAL_WIFI_JOIN_OK) {
        set_wifi_scan_status("");
        if (wifi_password) lv_textarea_set_text(wifi_password, "");
        if (active == wifi_password_scr || active == wifi_scan_scr) back_to_network(NULL);
        return;
    }
    const char *message;
    switch (res->joined) {
    case TERMINAL_WIFI_JOIN_INVALID_PASSWORD: message = "请输入 8–63 个字符，或 64 位十六进制密码。"; break;
    case TERMINAL_WIFI_JOIN_AUTH_FAILED: message = "认证失败，请核对密码后重试。"; break;
    case TERMINAL_WIFI_JOIN_SAVE_FAILED: message = "无法保存 Wi-Fi 设置，请重试。"; break;
    default: message = "连接失败，请检查信号后重试。"; break;
    }
    if (active == wifi_password_scr) lv_label_set_text(wifi_password_status, message);
    else set_wifi_scan_status(message);
}

static void select_network_mode(lv_event_t *event)
{
    terminal_network_mode_t mode = (terminal_network_mode_t)(uintptr_t)lv_event_get_user_data(event);
    if (queue_setting((set_cmd_t){.action = SET_NETWORK_MODE, .mode = mode})) {
        network_pending = true;
        network_save_failed = false;
        refresh_network_state();
    }
}

static void pair_device_clicked(lv_event_t *event)
{
    (void)event;
    if (busy || lv_obj_has_state(pair_button, LV_STATE_DISABLED)) return;
    if (!can_restart()) {
        lv_label_set_text(pair_status, "请先结束录音再配对。");
        return;
    }
    char *link = strdup(lv_textarea_get_text(pair_address));
    if (link) {
        if (queue_setting((set_cmd_t){.action = SET_PAIR_DEVICE, .pair_link = link})) {
            lv_obj_add_state(pair_button, LV_STATE_DISABLED);
            lv_label_set_text(pair_status, "正在配对…");
            return;
        }
        mbedtls_platform_zeroize(link, strlen(link));
        free(link);
    }
    lv_label_set_text(pair_status, "无法开始配对，请重试。");
}

static void back_to_connection(lv_event_t *event)
{
    (void)event;
    lv_screen_load(connection_scr);
}

static void pair_link_changed(lv_event_t *event)
{
    if (terminal_pair_link_invitation(lv_textarea_get_text(pair_address))) {
        pair_device_clicked(event);
    }
}

static void open_pair_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!pair_scr) {
        lv_obj_t *body = NULL;
        pair_scr = create_subpage_shell("配对设备", &body, back_to_connection, NULL);

        lv_obj_t *frame = eink_textarea(body, eink_body_font(), 50, EINK_RADIUS_CONTROL, &pair_address);
        lv_obj_set_width(frame, LV_PCT(100));
        lv_textarea_set_placeholder_text(pair_address, "完整配对链接");
        lv_textarea_set_max_length(pair_address, TERMINAL_PAIR_LINK_SIZE - 1);
        lv_obj_add_event_cb(pair_address, pair_link_changed, LV_EVENT_VALUE_CHANGED, NULL);

        pair_button = eink_outline_button(body, "配对并连接", eink_body_font(), 48, pair_device_clicked, NULL);
        lv_obj_set_width(pair_button, LV_PCT(100));
        pair_status = eink_label(body, "在管理页点击「添加设备」并复制链接。", eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(pair_status, LV_PCT(100));
        pair_keyboard = lv_keyboard_create(pair_scr);
        eink_style_keyboard(pair_keyboard);
        lv_obj_set_size(pair_keyboard, LV_PCT(100), 218);
        lv_keyboard_set_mode(pair_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(pair_keyboard, pair_address);
        lv_obj_add_event_cb(pair_keyboard, pair_device_clicked, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(pair_keyboard, back_to_connection, LV_EVENT_CANCEL, NULL);
    }
    lv_screen_load(pair_scr);
}

static void open_connection_page(lv_event_t *event)
{
    (void)event;
    if (!connection_scr) {
        lv_obj_t *body = NULL;
        connection_scr = create_subpage_shell("连接", &body, back_to_index, NULL);
        lv_obj_t *group = make_settings_card(body);
        const char *device_id = wintermute_device_id();
        const char *server = terminal_server_url();
        make_info_row(group, "设备名称", WINTERMUTE_DEVICE_NAME, NULL);
        add_inset_divider(group);
        make_info_row(group, "设备平台", "ESP32-S3", NULL);
        add_inset_divider(group);
        make_info_row(group, "设备 ID", device_id[0] ? device_id : "未生成", NULL);
        add_inset_divider(group);
        make_info_row(group, "服务地址", server[0] ? server : "未配置", NULL);
        lv_obj_t *button = eink_outline_button(body, "配对设备", eink_body_font(), 48, open_pair_page, NULL);
        lv_obj_set_width(button, LV_PCT(100));
    }
    lv_screen_load(connection_scr);
}

static void open_network_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!network_scr) {
        lv_obj_t *body = NULL;
        network_scr = create_subpage_shell("网络", &body, back_to_index, NULL);

        lv_obj_t *modes = lv_obj_create(body);
        eink_style_clean_box(modes);
        lv_obj_remove_flag(modes, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(modes, LV_PCT(100), 48);
        lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(modes, 8, LV_PART_MAIN);
        static const char *const options[] = {
            [TERMINAL_NETWORK_WIFI] = "Wi-Fi",
            [TERMINAL_NETWORK_CELLULAR] = "移动网络",
            [TERMINAL_NETWORK_OFF] = "关闭",
        };
        for (unsigned i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
            lv_obj_t *btn = eink_outline_button(modes, options[i], eink_body_font(), 48,
                                               select_network_mode, (void *)(uintptr_t)i);
            network_options[i] = btn;
            lv_obj_set_flex_grow(btn, 1);
            lv_obj_set_style_pad_hor(btn, 0, LV_PART_MAIN);
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(btn, 6, LV_PART_MAIN);
            lv_obj_t *check = eink_label(btn, LV_SYMBOL_OK, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
            lv_obj_move_to_index(check, 0);
        }

        lv_obj_t *group = make_settings_card(body);

        make_info_row(group, "网络状态", "", &network_status_val);
        add_inset_divider(group);
        make_info_row(group, "Wi-Fi 名称", "", &wifi_name_val);

        wifi_scan_button = eink_outline_button(body, "扫描 Wi-Fi", eink_body_font(), 48,
                                               open_wifi_scan, NULL);
        lv_obj_set_width(wifi_scan_button, LV_PCT(100));
    }
    refresh_network_state();
    lv_screen_load(network_scr);
}

static void open_device_app(lv_event_t *event)
{
    device_app_open((device_app_t)(uintptr_t)lv_event_get_user_data(event), connectors_scr);
}

static void open_connectors_page(lv_event_t *event)
{
    (void)event;
    if (!connectors_scr) {
        lv_obj_t *body = NULL;
        connectors_scr = create_subpage_shell("连接器", &body, back_to_index, NULL);
        for (unsigned i = 0; i < APP_COUNT; ++i) {
            lv_obj_t *row = make_category_row(body, app_titles[i], "", NULL, NULL);
            lv_obj_add_event_cb(row, open_device_app, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        }
    }
    lv_screen_load(connectors_scr);
}

static void open_appearance_page(lv_event_t *event)
{
    (void)event;
    if (!appearance_scr) {
        lv_obj_t *body = NULL;
        appearance_scr = create_subpage_shell("外观", &body, back_to_index, NULL);

        lv_obj_t *group = make_settings_card(body);

        make_info_row(group, "屏幕类型", "480×800 墨水屏", NULL);
        add_inset_divider(group);
        make_info_row(group, "色彩模式", "1-bit 黑白纸感", NULL);
        add_inset_divider(group);
        make_info_row(group, "刷新策略", "局刷 / 10 帧全刷", NULL);

        lv_obj_t *refresh_btn = eink_outline_button(body, "全刷屏幕（清除残影）", eink_body_font(), 56,
                                                   trigger_full_refresh, NULL);
        lv_obj_set_width(refresh_btn, LV_PCT(100));
    }
    lv_screen_load(appearance_scr);
}

static void load_collection_preferences(void)
{
    nvs_handle_t prefs;
    if (nvs_open(COLLECTION_PREFS_NAMESPACE, NVS_READONLY, &prefs) != ESP_OK) return;
    uint8_t enabled;
    if (nvs_get_u8(prefs, recording_upload_key, &enabled) == ESP_OK) {
        recording_auto_upload = enabled == 1;
    }
    if (nvs_get_u8(prefs, READING_PROGRESS_PREF, &enabled) == ESP_OK) {
        reading_progress_upload = enabled == 1;
    }
    nvs_close(prefs);
}

static bool save_collection_preference(const char *key, bool enabled)
{
    nvs_handle_t prefs;
    if (nvs_open(COLLECTION_PREFS_NAMESPACE, NVS_READWRITE, &prefs) != ESP_OK) return false;
    esp_err_t error = nvs_set_u8(prefs, key, enabled);
    if (error == ESP_OK) error = nvs_commit(prefs);
    nvs_close(prefs);
    return error == ESP_OK;
}

static void refresh_recording_upload_mode(void)
{
    lv_obj_set_state(recording_manual_button, LV_STATE_CHECKED, !recording_auto_upload);
    lv_obj_set_state(recording_auto_button, LV_STATE_CHECKED, recording_auto_upload);
}

static void select_recording_upload(lv_event_t *event)
{
    bool automatic = (uintptr_t)lv_event_get_user_data(event) != 0;
    if (automatic != recording_auto_upload && !save_collection_preference(recording_upload_key, automatic)) {
        lv_label_set_text(recording_upload_note, "录音上传设置保存失败，请重试。");
        return;
    }
    recording_auto_upload = automatic;
    refresh_recording_upload_mode();
    lv_label_set_text(recording_upload_note, recording_upload_hint);
}

static void toggle_reading_upload(lv_event_t *event)
{
    lv_obj_t *toggle = lv_event_get_target_obj(event);
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    if (enabled == reading_progress_upload) return;
    if (!save_collection_preference(READING_PROGRESS_PREF, enabled)) {
        lv_obj_set_state(toggle, LV_STATE_CHECKED, reading_progress_upload);
        lv_label_set_text(reading_upload_note, "阅读进度设置保存失败，请重试。");
        return;
    }
    reading_progress_upload = enabled;
    if (enabled) reading_progress_wake();
    lv_label_set_text(reading_upload_note, reading_upload_hint);
}

static void open_collection_page(lv_event_t *event)
{
    (void)event;
    if (!collection_scr) {
        load_collection_preferences();
        lv_obj_t *body = NULL;
        collection_scr = create_subpage_shell("信息采集", &body, back_to_index, NULL);

        eink_label(body, "录音采集", eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_t *recording = make_settings_card(body);
        lv_obj_t *upload = make_info_row(recording, "录音上传", NULL, NULL);
        lv_obj_t *choices = lv_obj_create(upload);
        eink_style_clean_box(choices);
        lv_obj_remove_flag(choices, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(choices, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(choices, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(choices, 8, LV_PART_MAIN);
        recording_manual_button = eink_pill_button(choices, "手动上传", eink_small_font(), 44,
                                                    false, select_recording_upload, NULL);
        recording_auto_button = eink_pill_button(choices, "自动上传", eink_small_font(), 44,
                                                  false, select_recording_upload, (void *)(uintptr_t)1);
        refresh_recording_upload_mode();
        recording_upload_note = make_collection_note(recording, recording_upload_hint);

        add_inset_divider(recording);
        make_info_row(recording, "录音状态", "就绪", &rec_status_val);
        add_inset_divider(recording);
        make_info_row(recording, "状态同步", "后台自动上报", NULL);
        add_inset_divider(recording);
        make_info_row(recording, "本地缓存", "SD 卡分段存储", NULL);

        eink_label(body, "阅读采集", eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_t *reading = make_settings_card(body);
        lv_obj_t *progress = make_info_row(reading, "阅读进度上传", NULL, NULL);
        lv_obj_t *toggle = lv_switch_create(progress);
        lv_obj_set_size(toggle, 68, 36);
        lv_obj_set_style_anim_duration(toggle, 0, LV_PART_MAIN);
        lv_obj_set_state(toggle, LV_STATE_CHECKED, reading_progress_upload);
        lv_obj_add_event_cb(toggle, toggle_reading_upload, LV_EVENT_VALUE_CHANGED, NULL);
        reading_upload_note = make_collection_note(reading, reading_upload_hint);
    }
    settings_screen_refresh_state();
    lv_screen_load(collection_scr);
}

static void refresh_storage_clicked(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    bool started = queue_setting((set_cmd_t){.action = SET_READ_STORAGE});
    lv_label_set_text(sd_status_val, started ? "正在读取…" : "暂时无法读取，请点刷新");
}

static void open_storage_page(lv_event_t *event)
{
    (void)event;
    if (!storage_scr) {
        lv_obj_t *body = NULL;
        storage_scr = create_subpage_shell("存储", &body, back_to_index, refresh_storage_clicked);

        eink_label(body, "内置闪存", eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_t *flash = make_settings_card(body);
        make_info_row(flash, "总容量", "-", &flash_capacity_val);

        eink_label(body, "SD 卡", eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_t *sd = make_settings_card(body);
        make_info_row(sd, "状态", "-", &sd_status_val);
        add_inset_divider(sd);
        make_info_row(sd, "总容量", "-", &sd_total_val);
        add_inset_divider(sd);
        make_info_row(sd, "已用", "-", &sd_used_val);
        add_inset_divider(sd);
        make_info_row(sd, "可用", "-", &sd_free_val);
    }
    lv_screen_load(storage_scr);
    refresh_storage_clicked(NULL);
}

static void create_index_screen(void)
{
    index_scr = lv_obj_create(NULL);
    eink_style_screen(index_scr);
    lv_obj_set_flex_flow(index_scr, LV_FLEX_FLOW_COLUMN);

    eink_top_bar_font(index_scr, "设置", eink_heading_font(), back_to_home, NULL);

    lv_obj_t *body = lv_obj_create(index_scr);
    eink_style_clean_box(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(body, EINK_PAD_X, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *group = make_settings_card(body);
    // Keep the pressed row's outline clear of the surrounding card border.
    lv_obj_set_style_pad_all(group, 6, LV_PART_MAIN);

    make_category_row(group, "连接", "", NULL, open_connection_page);
    add_inset_divider(group);
    make_category_row(group, "网络", "", &idx_network_val, open_network_page);
    add_inset_divider(group);
    make_category_row(group, "连接器", "5 项应用", NULL, open_connectors_page);
    add_inset_divider(group);
    make_category_row(group, "外观", "墨水屏", NULL, open_appearance_page);
    add_inset_divider(group);
    make_category_row(group, "存储", "", NULL, open_storage_page);
    add_inset_divider(group);
    make_category_row(group, "信息采集", "就绪", &idx_rec_val, open_collection_page);
}

void settings_screen_refresh_state(void)
{
    const char *rec = recorder_label_zh();

    refresh_network_state();
    if (idx_rec_val) lv_label_set_text(idx_rec_val, rec);
    if (rec_status_val) lv_label_set_text(rec_status_val, rec);
}

void settings_screen_open(lv_obj_t *home_screen)
{
    home_ref = home_screen;
    if (!index_scr) create_index_screen();
    settings_screen_refresh_state();
    lv_screen_load(index_scr);
}

void settings_screen_hide(void)
{
    lv_obj_t *active = lv_screen_active();
    if (wifi_password && active == wifi_password_scr) lv_textarea_set_text(wifi_password, "");
    if (home_ref &&
        (active == index_scr || active == connection_scr || active == pair_scr ||
         active == network_scr || active == wifi_scan_scr ||
         active == wifi_password_scr || active == connectors_scr ||
         active == appearance_scr || active == collection_scr || active == storage_scr)) {
        lv_screen_load(home_ref);
    }
}
