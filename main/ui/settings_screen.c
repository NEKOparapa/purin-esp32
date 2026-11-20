#include "settings_screen.h"
#include "board.h"
#include "device_apps_screen.h"
#include "eink_ui.h"
#include "esp_flash.h"
#include "lvgl_port.h"
#include "recorder.h"
#include "terminal_http.h"
#include "terminal_network.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

enum { WIFI_SCAN_LIMIT = 16 };
typedef enum {
    SET_FETCH_KEYS, SET_FETCH_MEMORY, SET_NETWORK_MODE, SET_WIFI_SCAN, SET_WIFI_JOIN, SET_READ_STORAGE
} set_action_t;

typedef struct {
    set_action_t action;
    terminal_network_mode_t mode;
    terminal_wifi_ap_t ap;
    char *password;
} set_cmd_t;

typedef struct {
    set_action_t action;
    bool ok;
    char error[128];
    cJSON *json;
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
static lv_obj_t *conn_scr;
static lv_obj_t *wifi_scan_scr;
static lv_obj_t *wifi_password_scr;
static lv_obj_t *connectors_scr;
static lv_obj_t *keys_scr;
static lv_obj_t *memory_scr;
static lv_obj_t *appearance_scr;
static lv_obj_t *collection_scr;
static lv_obj_t *storage_scr;

static lv_obj_t *idx_conn_val;
static lv_obj_t *idx_rec_val;
static lv_obj_t *conn_status_val;
static lv_obj_t *conn_device_val;
static lv_obj_t *conn_id_val;
static lv_obj_t *conn_url_val;
static lv_obj_t *conn_wifi_val;
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
static lv_obj_t *keys_body;
static lv_obj_t *memory_body;
static lv_obj_t *flash_capacity_val;
static lv_obj_t *sd_status_val;
static lv_obj_t *sd_total_val;
static lv_obj_t *sd_used_val;
static lv_obj_t *sd_free_val;

static QueueHandle_t cmds, results;
static bool busy;
static bool network_pending;
static bool network_save_failed;

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

static bool is_secure_server(void)
{
    return strncmp(WINTERMUTE_SERVER_URL, "https://", 8) == 0 ||
           strncmp(WINTERMUTE_SERVER_URL, "http://localhost", 16) == 0 ||
           strncmp(WINTERMUTE_SERVER_URL, "http://127.0.0.1", 16) == 0;
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
        } else if (terminal_network_mode() == TERMINAL_NETWORK_OFF) {
            strlcpy(res.error, "请先在网络设置中开启连接", sizeof(res.error));
        } else if (!terminal_configured()) {
            strlcpy(res.error, "请先在固件中配置服务连接", sizeof(res.error));
        } else if (cmd.action == SET_FETCH_KEYS && !is_secure_server()) {
            strlcpy(res.error, "查看接口密钥需要 HTTPS 或本机连接", sizeof(res.error));
        } else if (terminal_network_acquire() != ESP_OK) {
            strlcpy(res.error, "无法联网，请在网络设置中查看状态", sizeof(res.error));
        } else {
            const char *route =
                cmd.action == SET_FETCH_KEYS ? "/v1/settings/api-keys" : "/v1/memory/schedule";
            int code = 0;
            res.json = terminal_http_json_bounded(res.error, sizeof(res.error), HTTP_METHOD_GET,
                                                  route, NULL, 200, 16384, &code);
            if (res.json) {
                res.ok = true;
            } else if (code == 404) {
                strlcpy(res.error,
                        cmd.action == SET_FETCH_KEYS ? "当前后端不支持接口密钥查询"
                                                     : "后端尚未配置记忆整理目录",
                        sizeof(res.error));
            } else if (!res.error[0]) {
                strlcpy(res.error, "无法读取后端配置，请重试", sizeof(res.error));
            }
            terminal_network_release();
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
    lv_obj_t *v = eink_label(row, val ? val : "", eink_small_font(), LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    if (val_out) *val_out = v;
    return row;
}

static lv_obj_t *make_note_card(lv_obj_t *parent, const char *title, const char *body_text)
{
    lv_obj_t *card = make_settings_card(parent);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);

    if (title && *title) {
        lv_obj_t *t = eink_label(card, title, eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(t, LV_PCT(100));
    }
    if (body_text && *body_text) {
        lv_obj_t *b = eink_label(card, body_text, eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(b, 6, LV_PART_MAIN);
    }
    return card;
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

static void refresh_keys_clicked(lv_event_t *event)
{
    (void)event;
    if (!keys_body) return;
    lv_obj_clean(keys_body);
    bool started = queue_setting((set_cmd_t){.action = SET_FETCH_KEYS});
    make_note_card(keys_body, NULL, started ? "正在读取接口密钥状态…" : "当前无法读取，请稍后刷新。");
}

static void refresh_memory_clicked(lv_event_t *event)
{
    (void)event;
    if (!memory_body) return;
    lv_obj_clean(memory_body);
    bool started = queue_setting((set_cmd_t){.action = SET_FETCH_MEMORY});
    make_note_card(memory_body, NULL, started ? "正在读取记忆整理计划…" : "当前无法读取，请稍后刷新。");
}

static void render_keys_result(const set_res_t *res)
{
    if (!keys_body) return;
    lv_obj_clean(keys_body);
    if (!res->ok || !res->json) {
        make_note_card(keys_body, "无法读取密钥", res->error);
        return;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(res->json, "items");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
        make_note_card(keys_body, "暂无接口密钥", "后端没有可管理的接口密钥。");
        return;
    }

    lv_obj_t *group = make_settings_card(keys_body);

    int count = cJSON_GetArraySize(items);
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const cJSON *usages = cJSON_GetObjectItemCaseSensitive(item, "usages");
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
        bool configured = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "configured"));
        const char *masked = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "masked"));

        char title[96] = {0};
        if (cJSON_IsArray(usages) && cJSON_GetArraySize(usages) > 0) {
            int n = cJSON_GetArraySize(usages);
            for (int j = 0; j < n; ++j) {
                const char *u = cJSON_GetStringValue(cJSON_GetArrayItem(usages, j));
                if (!u) continue;
                if (title[0]) strlcat(title, "、", sizeof(title));
                strlcat(title, u, sizeof(title));
            }
        }
        if (!title[0]) strlcpy(title, id ? id : "密钥", sizeof(title));

        char val[48];
        if (configured && masked && *masked) {
            snprintf(val, sizeof(val), "已配置 %s", masked);
        } else if (configured) {
            strlcpy(val, "已配置", sizeof(val));
        } else {
            strlcpy(val, "未配置", sizeof(val));
        }

        make_info_row(group, title, val, NULL);
        if (i + 1 < count) add_inset_divider(group);
    }

    make_note_card(keys_body, NULL, "已保存的密钥仅显示掩码；如需更换请在手机端或桌面端操作。");
}

static void render_memory_result(const set_res_t *res)
{
    if (!memory_body) return;
    lv_obj_clean(memory_body);
    if (!res->ok || !res->json) {
        make_note_card(memory_body, "无法读取整理计划", res->error);
        return;
    }
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(res->json, "interval_days");
    const char *local_time =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res->json, "local_time"));
    const char *tz = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res->json, "timezone"));
    const char *next_at =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(res->json, "next_run_at"));

    char days_buf[32] = "-";
    if (cJSON_IsNumber(days)) snprintf(days_buf, sizeof(days_buf), "%d 天", days->valueint);

    char time_buf[64] = "-";
    if (local_time && tz)
        snprintf(time_buf, sizeof(time_buf), "%s (%s)", local_time, tz);
    else if (local_time)
        strlcpy(time_buf, local_time, sizeof(time_buf));

    char next_buf[32] = "-";
    if (next_at && *next_at) {
        snprintf(next_buf, sizeof(next_buf), "%.16s", next_at);
        for (char *p = next_buf; *p; ++p) {
            if (*p == 'T') *p = ' ';
        }
    }

    lv_obj_t *group = make_settings_card(memory_body);

    make_info_row(group, "间隔天数", days_buf, NULL);
    add_inset_divider(group);
    make_info_row(group, "运行时间", time_buf, NULL);
    add_inset_divider(group);
    make_info_row(group, "下次整理", next_buf, NULL);

    make_note_card(memory_body, NULL,
                   "同一账号的所有设备共用此记忆整理计划，可在手机端或桌面端修改。");
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
    if (idx_conn_val && strcmp(lv_label_get_text(idx_conn_val), mode_names[mode])) {
        lv_label_set_text(idx_conn_val, mode_names[mode]);
    }
    if (!conn_status_val) return;

    char ssid[TERMINAL_WIFI_SSID_SIZE];
    bool configured = terminal_wifi_saved_ssid(ssid);
    char name[TERMINAL_WIFI_SSID_SIZE];
    if (configured) ssid_label(name, ssid);
    const char *saved_name = configured ? name : "未配置";
    if (strcmp(lv_label_get_text(conn_wifi_val), saved_name)) {
        lv_label_set_text(conn_wifi_val, saved_name);
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
    if (strcmp(lv_label_get_text(conn_status_val), status)) {
        lv_label_set_text(conn_status_val, status);
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
        } else if (res.action == SET_FETCH_KEYS)
            render_keys_result(&res);
        else
            render_memory_result(&res);
        cJSON_Delete(res.json);
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
    lv_screen_load(conn_scr);
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

static void open_conn_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!conn_scr) {
        lv_obj_t *body = NULL;
        conn_scr = create_subpage_shell("网络", &body, back_to_index, NULL);

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

        make_info_row(group, "连接状态", "", &conn_status_val);
        add_inset_divider(group);
        make_info_row(group, "Wi-Fi 名称", "", &conn_wifi_val);
        add_inset_divider(group);
        make_info_row(group, "设备名称", "", &conn_device_val);
        add_inset_divider(group);
        make_info_row(group, "设备 ID", "", &conn_id_val);
        add_inset_divider(group);
        make_info_row(group, "服务地址", "", &conn_url_val);

        wifi_scan_button = eink_outline_button(body, "扫描 Wi-Fi", eink_body_font(), 48,
                                               open_wifi_scan, NULL);
        lv_obj_set_width(wifi_scan_button, LV_PCT(100));
    }
    settings_screen_refresh_state();
    lv_screen_load(conn_scr);
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

static void open_keys_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!keys_scr) {
        keys_scr = create_subpage_shell("接口密钥", &keys_body, back_to_index, refresh_keys_clicked);
    }
    lv_screen_load(keys_scr);
    refresh_keys_clicked(NULL);
}

static void open_memory_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!memory_scr) {
        memory_scr = create_subpage_shell("记忆整理", &memory_body, back_to_index, refresh_memory_clicked);
    }
    lv_screen_load(memory_scr);
    refresh_memory_clicked(NULL);
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

static void open_collection_page(lv_event_t *event)
{
    (void)event;
    if (!collection_scr) {
        lv_obj_t *body = NULL;
        collection_scr = create_subpage_shell("信息采集", &body, back_to_index, NULL);

        lv_obj_t *group = make_settings_card(body);

        make_info_row(group, "录音状态", "就绪", &rec_status_val);
        add_inset_divider(group);
        make_info_row(group, "状态同步", "后台自动上报", NULL);
        add_inset_divider(group);
        make_info_row(group, "本地缓存", "SD 卡分段存储", NULL);

        make_note_card(body, "按键采集说明",
                       "长按 BOOT 键开始或停止录音。\n录音过程中短按 BOOT 键可选择时长并上传音频。");
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

    make_category_row(group, "网络", "", &idx_conn_val, open_conn_page);
    add_inset_divider(group);
    make_category_row(group, "连接器", "5 项应用", NULL, open_connectors_page);
    add_inset_divider(group);
    make_category_row(group, "接口密钥", "查看", NULL, open_keys_page);
    add_inset_divider(group);
    make_category_row(group, "记忆整理", "计划", NULL, open_memory_page);
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
    if (conn_device_val) lv_label_set_text(conn_device_val, WINTERMUTE_DEVICE_NAME);
    if (conn_id_val) lv_label_set_text(conn_id_val, wintermute_device_id());
    if (conn_url_val) {
        lv_label_set_text(conn_url_val,
                          WINTERMUTE_SERVER_URL[0] ? WINTERMUTE_SERVER_URL : "未配置");
    }
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
        (active == index_scr || active == conn_scr || active == wifi_scan_scr ||
         active == wifi_password_scr || active == connectors_scr ||
         active == keys_scr || active == memory_scr || active == appearance_scr ||
         active == collection_scr || active == storage_scr)) {
        lv_screen_load(home_ref);
    }
}
