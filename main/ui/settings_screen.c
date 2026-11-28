#include "settings_screen.h"
#include "board.h"
#include "device_apps.h"
#include "eink_ui.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "lvgl_port.h"
#include "reading_progress.h"
#include "recorder.h"
#include "qr_scanner.h"
#include "terminal_http.h"
#include "terminal_network.h"
#include "terminal_pairing.h"
#include "terminal_tools.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

enum {
    WIFI_SCAN_LIMIT = 16,
    ONLINE_DEVICE_LIMIT = 24,
    QR_PREVIEW_W = 384,
    QR_PREVIEW_H = 288,
    QR_PREVIEW_STRIDE = QR_PREVIEW_W / 8,
    QR_PREVIEW_PALETTE = 8,
    QR_PREVIEW_BYTES = QR_PREVIEW_STRIDE * QR_PREVIEW_H,
};
typedef enum {
    SET_NETWORK_MODE, SET_WIFI_SCAN, SET_WIFI_JOIN, SET_READ_STORAGE, SET_PAIR_DEVICE,
    SET_QR_PAIR, SET_READ_ONLINE_DEVICES
} set_action_t;

typedef struct {
    char device_id[64];
    char name[84];
    char platform[24];
    bool is_current;
} online_device_info_t;

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
    bool canceled;
    char error[128];
    size_t ap_count;
    terminal_wifi_ap_t aps[WIFI_SCAN_LIMIT];
    terminal_wifi_join_result_t joined;
    uint32_t flash_bytes;
    uint64_t sd_total_bytes;
    uint64_t sd_free_bytes;
    esp_err_t sd_error;
    size_t online_count;
    online_device_info_t *online_devices;
} set_res_t;

static lv_obj_t *home_ref;
static lv_obj_t *index_scr;
static lv_obj_t *connection_scr;
static lv_obj_t *pair_scr;
static lv_obj_t *pair_address;
static lv_obj_t *pair_keyboard;
static lv_obj_t *pair_button;
static lv_obj_t *pair_status;
static lv_obj_t *qr_pair_scr;
static lv_obj_t *qr_preview_img;
static lv_obj_t *qr_pair_status;
static lv_obj_t *qr_retry_button;
static lv_image_dsc_t qr_preview_dsc;
static uint8_t *qr_preview_front;
static uint8_t *qr_preview_stage;
static SemaphoreHandle_t qr_preview_lock;
static bool qr_preview_dirty;
static char qr_live_status[128];
static atomic_bool qr_scan_cancel;
static lv_obj_t *network_scr;
static lv_obj_t *wifi_scan_scr;
static lv_obj_t *wifi_password_scr;
static lv_obj_t *connectors_scr;
static lv_obj_t *connector_switches[APP_COUNT], *connector_note;
static lv_obj_t *appearance_scr;
static lv_obj_t *collection_scr;
static lv_obj_t *storage_scr;

static lv_obj_t *idx_network_val;
static lv_obj_t *idx_rec_val;
static lv_obj_t *idx_connectors_val;
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
static lv_obj_t *online_devices_title;
static lv_obj_t *online_devices_card;
static online_device_info_t *rendered_online;
static size_t rendered_online_count;
static bool rendered_online_valid;
static bool rendered_online_ok;
static char rendered_online_error[128];
static int64_t online_next_refresh_us;

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
static void render_online_devices(set_res_t *res);
static void request_online_devices_refresh(bool show_loading);
static bool has_backend_credentials(void);
static bool can_restart(void);
static void ssid_label(char out[TERMINAL_WIFI_SSID_SIZE], const char *ssid);

static bool is_purin_pair_qr(const char *payload)
{
    if (!payload) return false;
    while (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n') ++payload;
    char trimmed[TERMINAL_PAIR_LINK_SIZE];
    strlcpy(trimmed, payload, sizeof(trimmed));
    size_t len = strlen(trimmed);
    while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t' ||
                       trimmed[len - 1] == '\r' || trimmed[len - 1] == '\n')) {
        trimmed[--len] = '\0';
    }
    return terminal_pair_link_invitation(trimmed) != NULL;
}

static void publish_qr_live_status(const char *status)
{
    if (!qr_preview_lock || !status) return;
    xSemaphoreTake(qr_preview_lock, portMAX_DELAY);
    strlcpy(qr_live_status, status, sizeof(qr_live_status));
    xSemaphoreGive(qr_preview_lock);
}

static const char *camera_start_error(board_camera_stage_t stage, esp_err_t err)
{
    if (err == ESP_ERR_NO_MEM) return "内存不足，请点重试。";
    switch (stage) {
    case BOARD_CAMERA_POWER:
        if (err == ESP_ERR_NOT_FOUND) return "摄像头未连接，供电芯片也未响应。请点重试。";
        return "摄像头供电设置失败，请点重试。";
    case BOARD_CAMERA_USB_MUX:
        return "切换摄像头 USB 通路失败，请点重试。";
    case BOARD_CAMERA_USB_HOST:
        return "USB 主机启动失败，请点重试。";
    case BOARD_CAMERA_DETECT:
        if (err == ESP_ERR_NOT_SUPPORTED) return "已发现 USB 设备，未识别到 UVC 摄像头。";
        if (err == ESP_ERR_INVALID_STATE) return "未检测到摄像头，供电芯片未进入 OTG 模式。请用电池供电后重试。";
        return "USB 未检测到摄像头。请检查摄像头连接，或改用链接配对。";
    case BOARD_CAMERA_STREAM:
        return "已检测到摄像头，视频流启动失败。请点重试。";
    default:
        return "摄像头启动失败，请点重试。";
    }
}

static void run_qr_pair(set_res_t *res)
{
    qr_scanner_handle_t scanner = qr_scanner_create();
    uint8_t *preview_tmp = qr_preview_stage
        ? heap_caps_malloc(QR_PREVIEW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!scanner) {
        free(preview_tmp);
        strlcpy(res->error, "内存不足，请点重试。", sizeof(res->error));
        return;
    }
    if (atomic_load(&qr_scan_cancel)) {
        free(preview_tmp);
        qr_scanner_destroy(scanner);
        res->canceled = true;
        return;
    }

    board_camera_stage_t stage;
    esp_err_t cam_err = board_camera_open_with_status(&stage);
    if (cam_err != ESP_OK) {
        free(preview_tmp);
        qr_scanner_destroy(scanner);
        if (atomic_load(&qr_scan_cancel)) {
            res->canceled = true;
            return;
        }
        strlcpy(res->error, camera_start_error(stage, cam_err), sizeof(res->error));
        return;
    }

    publish_qr_live_status("请将管理页的配对二维码对准摄像头…");
    const int64_t deadline = esp_timer_get_time() + 60LL * 1000000;
    int64_t last_preview_us = 0;
    bool warned_other_qr = false;
    char link[TERMINAL_PAIR_LINK_SIZE] = {0};

    while (!atomic_load(&qr_scan_cancel) && esp_timer_get_time() < deadline) {
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        esp_err_t err = board_camera_capture_jpeg(&jpeg, &jpeg_len, 1000);
        if (err == ESP_ERR_TIMEOUT) continue;
        if (err != ESP_OK) {
            strlcpy(res->error, "摄像头连接中断，请点重试。", sizeof(res->error));
            break;
        }

        const int64_t now = esp_timer_get_time();
        const bool want_preview = preview_tmp && qr_preview_lock && (now - last_preview_us >= 450000);
        bool any_qr = false;
        char raw_qr[TERMINAL_PAIR_LINK_SIZE + 16] = {0};
        err = qr_scanner_decode_jpeg(scanner, jpeg, jpeg_len,
                                     want_preview ? preview_tmp : NULL,
                                     QR_PREVIEW_W, QR_PREVIEW_H,
                                     raw_qr, sizeof(raw_qr),
                                     is_purin_pair_qr, &any_qr);
        free(jpeg);

        if (err == ESP_OK && want_preview) {
            xSemaphoreTake(qr_preview_lock, portMAX_DELAY);
            memcpy(qr_preview_stage, preview_tmp, QR_PREVIEW_BYTES);
            qr_preview_dirty = true;
            xSemaphoreGive(qr_preview_lock);
            last_preview_us = now;
        }

        if (raw_qr[0]) {
            const char *p = raw_qr;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
            strlcpy(link, p, sizeof(link));
            size_t len = strlen(link);
            while (len > 0 && (link[len - 1] == ' ' || link[len - 1] == '\t' ||
                               link[len - 1] == '\r' || link[len - 1] == '\n')) {
                link[--len] = '\0';
            }
            mbedtls_platform_zeroize(raw_qr, sizeof(raw_qr));
            break;
        }
        if (any_qr && !warned_other_qr) {
            warned_other_qr = true;
            publish_qr_live_status("请扫描 Purin 管理页「添加设备」生成的二维码。");
        }
    }

    // Release USB camera, mux, and OTG power before redeeming the invitation over HTTPS.
    board_camera_close();
    free(preview_tmp);
    qr_scanner_destroy(scanner);

    if (atomic_load(&qr_scan_cancel)) {
        mbedtls_platform_zeroize(link, sizeof(link));
        res->canceled = true;
        return;
    }
    if (link[0]) {
        (void)board_vibration_pulse(0);
        publish_qr_live_status("已识别配对码，正在连接配对…");
        res->ok = terminal_pair_device(link, res->error, sizeof(res->error));
        mbedtls_platform_zeroize(link, sizeof(link));
        return;
    }
    if (!res->error[0]) {
        strlcpy(res->error, "扫码超时，点右上角的刷新图标重新扫码。", sizeof(res->error));
    }
}

static void sanitize_label(char *out, size_t size, const char *src)
{
    strlcpy(out, src ? src : "", size);
    for (char *c = out; *c; ++c) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = '?';
    }
}

static const char *platform_label_zh(const char *platform, const char *device_type)
{
    if (platform) {
        if (!strcmp(platform, "windows")) return "Windows";
        if (!strcmp(platform, "macos")) return "macOS";
        if (!strcmp(platform, "android")) return "Android";
        if (!strcmp(platform, "ios")) return "iOS";
        if (!strcmp(platform, "esp32")) return "ESP32";
    }
    if (device_type) {
        if (!strcmp(device_type, "desktop")) return "桌面端";
        if (!strcmp(device_type, "mobile")) return "手机";
        if (!strcmp(device_type, "esp32")) return "ESP32";
    }
    return "设备";
}

static void fetch_online_devices(set_res_t *res)
{
    if (!terminal_configured()) {
        strlcpy(res->error,
                terminal_network_mode() == TERMINAL_NETWORK_OFF
                    ? "网络已关闭，请先开启网络"
                    : "请先在网络设置中连接网络",
                sizeof(res->error));
        return;
    }
    if (terminal_network_acquire() != ESP_OK) {
        strlcpy(res->error, "网络未连接，请检查网络设置", sizeof(res->error));
        return;
    }
    int status = 0;
    char http_err[96] = {0};
    cJSON *json = terminal_http_json_bounded(http_err, sizeof(http_err), HTTP_METHOD_GET,
                                             "/v1/devices/online", NULL, 200, 16 * 1024, &status);
    terminal_network_release();
    if (!json) {
        const char *msg = "读取失败，请点刷新重试";
        if (status == 401) msg = "设备凭据无效，请重新配对";
        else if (status == 404) msg = "后端不支持在线设备查询";
        strlcpy(res->error, msg, sizeof(res->error));
        return;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(json);
        strlcpy(res->error, "服务返回格式无效", sizeof(res->error));
        return;
    }
    int total = cJSON_GetArraySize(items);
    if (total < 0) total = 0;
    size_t cap = (size_t)total > ONLINE_DEVICE_LIMIT ? ONLINE_DEVICE_LIMIT : (size_t)total;
    if (cap > 0) {
        res->online_devices = calloc(cap, sizeof(*res->online_devices));
        if (!res->online_devices) {
            cJSON_Delete(json);
            strlcpy(res->error, "内存不足，请点刷新重试", sizeof(res->error));
            return;
        }
    }
    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (count >= cap) break;
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "device_id"));
        if (!id || !*id) continue;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "name"));
        const char *platform = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "platform"));
        const char *dtype = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "device_type"));
        online_device_info_t *dst = &res->online_devices[count++];
        sanitize_label(dst->device_id, sizeof(dst->device_id), id);
        sanitize_label(dst->name, sizeof(dst->name), (name && *name) ? name : id);
        strlcpy(dst->platform, platform_label_zh(platform, dtype), sizeof(dst->platform));
        dst->is_current = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_current"));
    }
    cJSON_Delete(json);
    res->online_count = count;
    res->ok = true;
}

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
        } else if (cmd.action == SET_QR_PAIR) {
            run_qr_pair(&res);
        } else if (cmd.action == SET_READ_ONLINE_DEVICES) {
            fetch_online_devices(&res);
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

static lv_obj_t *make_navigation_row(lv_obj_t *parent)
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
    if (qr_pair_scr && qr_preview_lock) {
        bool refresh_preview = false;
        char status_copy[sizeof(qr_live_status)] = {0};
        xSemaphoreTake(qr_preview_lock, portMAX_DELAY);
        if (qr_live_status[0]) {
            strlcpy(status_copy, qr_live_status, sizeof(status_copy));
            qr_live_status[0] = '\0';
        }
        if (qr_preview_dirty && qr_preview_front && lv_screen_active() == qr_pair_scr) {
            memcpy(qr_preview_front + QR_PREVIEW_PALETTE, qr_preview_stage, QR_PREVIEW_BYTES);
            qr_preview_dirty = false;
            refresh_preview = true;
        }
        xSemaphoreGive(qr_preview_lock);
        if (status_copy[0] && qr_pair_status) lv_label_set_text(qr_pair_status, status_copy);
        if (refresh_preview && qr_preview_img) lv_obj_invalidate(qr_preview_img);
    }

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
        } else if (res.action == SET_QR_PAIR) {
            if (!res.canceled && qr_pair_status) {
                lv_label_set_text(qr_pair_status, res.ok ? "已配对，正在重启连接…" : res.error);
                if (res.ok) {
                    lv_timer_create(restart_after_pairing, 1000, NULL);
                } else if (qr_retry_button) {
                    lv_obj_remove_state(qr_retry_button, LV_STATE_DISABLED);
                }
            } else if (qr_retry_button) {
                lv_obj_remove_state(qr_retry_button, LV_STATE_DISABLED);
            }
        } else if (res.action == SET_READ_ONLINE_DEVICES) {
            render_online_devices(&res);
            free(res.online_devices);
        }
    }
    if (connection_scr && lv_screen_active() == connection_scr &&
        has_backend_credentials() && !busy &&
        esp_timer_get_time() >= online_next_refresh_us) {
        request_online_devices_refresh(false);
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
        eink_refresh_button(right_slot, on_refresh);
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
        lv_obj_t *row = make_navigation_row(wifi_scan_list);
        lv_obj_t *ssid = eink_label(row, name, eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(ssid, 1);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_MODE_DOTS);
        eink_label(row, details, eink_small_font(), LV_TEXT_ALIGN_RIGHT);
        eink_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
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

static void open_qr_pair_page(lv_event_t *event);

static void back_from_qr_pair(lv_event_t *event)
{
    (void)event;
    atomic_store(&qr_scan_cancel, true);
    lv_screen_load(connection_scr);
}

static void clear_qr_preview(void)
{
    if (!qr_preview_front) return;
    lv_color32_t *palette = (lv_color32_t *)qr_preview_front;
    palette[0] = lv_color_to_32(lv_color_black(), LV_OPA_COVER);
    palette[1] = lv_color_to_32(lv_color_white(), LV_OPA_COVER);
    memset(qr_preview_front + QR_PREVIEW_PALETTE, 0xFF, QR_PREVIEW_BYTES);
    if (qr_preview_img) lv_obj_invalidate(qr_preview_img);
}

static void start_qr_pair_scan(void)
{
    ensure_worker();
    clear_qr_preview();
    if (!can_restart()) {
        lv_label_set_text(qr_pair_status, "请先结束录音再扫码配对。");
        if (qr_retry_button) lv_obj_remove_state(qr_retry_button, LV_STATE_DISABLED);
        return;
    }
    if (terminal_network_mode() == TERMINAL_NETWORK_OFF) {
        lv_label_set_text(qr_pair_status, "网络已关闭，请先在网络设置中开启连接。");
        if (qr_retry_button) lv_obj_remove_state(qr_retry_button, LV_STATE_DISABLED);
        return;
    }
    if (qr_preview_lock) {
        xSemaphoreTake(qr_preview_lock, portMAX_DELAY);
        qr_preview_dirty = false;
        qr_live_status[0] = '\0';
        xSemaphoreGive(qr_preview_lock);
    }
    atomic_store(&qr_scan_cancel, false);
    if (queue_setting((set_cmd_t){.action = SET_QR_PAIR})) {
        if (qr_retry_button) lv_obj_add_state(qr_retry_button, LV_STATE_DISABLED);
        lv_label_set_text(qr_pair_status, "正在启动摄像头…");
    } else {
        lv_label_set_text(qr_pair_status, "设备正忙，请稍后点刷新图标重新扫码。");
        if (qr_retry_button) lv_obj_remove_state(qr_retry_button, LV_STATE_DISABLED);
    }
}

static void retry_qr_pair_clicked(lv_event_t *event)
{
    (void)event;
    if (busy) return;
    start_qr_pair_scan();
}

static void open_qr_pair_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!qr_preview_lock) qr_preview_lock = xSemaphoreCreateMutex();
    if (!qr_preview_front) {
        qr_preview_front = heap_caps_malloc(QR_PREVIEW_PALETTE + QR_PREVIEW_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!qr_preview_stage) {
        qr_preview_stage = heap_caps_malloc(QR_PREVIEW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!qr_pair_scr) {
        qr_pair_scr = lv_obj_create(NULL);
        eink_style_screen(qr_pair_scr);
        lv_obj_set_flex_flow(qr_pair_scr, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *right_slot = NULL;
        eink_top_bar_font(qr_pair_scr, "扫码配对", eink_heading_font(), back_from_qr_pair, &right_slot);
        if (right_slot) {
            qr_retry_button = eink_refresh_button(right_slot, retry_qr_pair_clicked);
        }

        lv_obj_t *body = lv_obj_create(qr_pair_scr);
        eink_style_clean_box(body);
        lv_obj_set_width(body, LV_PCT(100));
        lv_obj_set_flex_grow(body, 1);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(body, EINK_PAD_X, LV_PART_MAIN);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, 18, LV_PART_MAIN);

        lv_obj_t *card = make_settings_card(body);
        lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        if (qr_preview_front) {
            clear_qr_preview();
            qr_preview_dsc = (lv_image_dsc_t){
                .header = {
                    .magic = LV_IMAGE_HEADER_MAGIC,
                    .cf = LV_COLOR_FORMAT_I1,
                    .w = QR_PREVIEW_W,
                    .h = QR_PREVIEW_H,
                    .stride = QR_PREVIEW_STRIDE,
                },
                .data_size = QR_PREVIEW_PALETTE + QR_PREVIEW_BYTES,
                .data = qr_preview_front,
            };
            qr_preview_img = lv_image_create(card);
            lv_image_set_src(qr_preview_img, &qr_preview_dsc);
        }

        qr_pair_status = eink_label(body, "", eink_body_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(qr_pair_status, LV_PCT(100));
        lv_label_set_long_mode(qr_pair_status, LV_LABEL_LONG_MODE_WRAP);

        lv_obj_t *hint = eink_label(body, "在管理页点击「添加设备」，将生成的配对二维码置于取景框中央。",
                                    eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_screen_load(qr_pair_scr);
    start_qr_pair_scan();
}

static void open_pair_page(lv_event_t *event)
{
    (void)event;
    ensure_worker();
    if (!pair_scr) {
        lv_obj_t *body = NULL;
        pair_scr = lv_obj_create(NULL);
        eink_style_screen(pair_scr);
        lv_obj_set_flex_flow(pair_scr, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *right_slot = NULL;
        eink_top_bar_font(pair_scr, "配对设备", eink_heading_font(), back_to_connection, &right_slot);
        if (right_slot) {
            lv_obj_t *scan_btn = eink_outline_button(right_slot, "扫码", eink_small_font(), 40,
                                                     open_qr_pair_page, NULL);
            lv_obj_set_style_pad_hor(scan_btn, 14, LV_PART_MAIN);
        }

        body = lv_obj_create(pair_scr);
        eink_style_clean_box(body);
        lv_obj_set_width(body, LV_PCT(100));
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_all(body, EINK_PAD_X, LV_PART_MAIN);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, 18, LV_PART_MAIN);

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

static bool has_backend_credentials(void)
{
    const char *server = terminal_server_url();
    return terminal_is_paired() ||
           (terminal_device_token()[0] != '\0' &&
            (!strncmp(server, "https://", 8) || !strncmp(server, "http://", 7)));
}

static void make_online_device_row(lv_obj_t *parent, const online_device_info_t *dev)
{
    lv_obj_t *row = lv_obj_create(parent);
    eink_style_clean_box(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 72, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);

    lv_obj_t *left = lv_obj_create(row);
    eink_style_clean_box(left);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, LV_PART_MAIN);

    char title[108];
    if (dev->is_current) {
        snprintf(title, sizeof(title), "%s（本机）", dev->name);
    } else {
        strlcpy(title, dev->name, sizeof(title));
    }
    lv_obj_t *name_lbl = eink_label(left, title, eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(name_lbl, LV_PCT(100));
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *id_lbl = eink_label(left, dev->device_id, eink_small_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(id_lbl, LV_PCT(100));
    lv_label_set_long_mode(id_lbl, LV_LABEL_LONG_MODE_WRAP);

    char meta[40];
    snprintf(meta, sizeof(meta), "%s\n在线", dev->platform);
    lv_obj_t *right_lbl = eink_label(row, meta, eink_small_font(), LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_text_line_space(right_lbl, 4, LV_PART_MAIN);
}

static void render_online_devices(set_res_t *res)
{
    if (!online_devices_card || !online_devices_title) return;
    size_t bytes = res->online_count * sizeof(*res->online_devices);
    if (rendered_online_valid && rendered_online_ok == res->ok) {
        if (!res->ok && !strcmp(rendered_online_error, res->error)) return;
        if (res->ok && rendered_online_count == res->online_count &&
            (bytes == 0 || (rendered_online && res->online_devices &&
                            memcmp(rendered_online, res->online_devices, bytes) == 0))) {
            return;
        }
    }

    rendered_online_valid = true;
    rendered_online_ok = res->ok;
    rendered_online_count = res->online_count;
    strlcpy(rendered_online_error, res->error, sizeof(rendered_online_error));
    free(rendered_online);
    rendered_online = NULL;
    if (res->ok && bytes > 0) {
        rendered_online = res->online_devices;
        res->online_devices = NULL;
    }

    if (res->ok) {
        char heading[32];
        snprintf(heading, sizeof(heading), "在线设备（%u）", (unsigned)res->online_count);
        lv_label_set_text(online_devices_title, heading);
    } else {
        lv_label_set_text(online_devices_title, "在线设备");
    }

    lv_obj_clean(online_devices_card);
    if (!res->ok) {
        make_info_row(online_devices_card, "状态",
                      res->error[0] ? res->error : "读取失败，请点刷新重试", NULL);
        return;
    }
    if (res->online_count == 0) {
        make_info_row(online_devices_card, "状态", "暂无在线设备", NULL);
        return;
    }
    for (size_t i = 0; i < res->online_count; ++i) {
        make_online_device_row(online_devices_card, &rendered_online[i]);
        if (i + 1 < res->online_count) add_inset_divider(online_devices_card);
    }
}

static void request_online_devices_refresh(bool show_loading)
{
    if (!online_devices_card || !has_backend_credentials()) return;
    ensure_worker();
    online_next_refresh_us = esp_timer_get_time() + 10 * 1000000LL;
    bool started = queue_setting((set_cmd_t){.action = SET_READ_ONLINE_DEVICES});
    if (show_loading) {
        rendered_online_valid = false;
        lv_obj_clean(online_devices_card);
        make_info_row(online_devices_card, "状态",
                      started ? "正在读取…" : "暂时无法读取，请点刷新", NULL);
    }
}

static void refresh_online_devices_clicked(lv_event_t *event)
{
    (void)event;
    request_online_devices_refresh(true);
}

static void open_connection_page(lv_event_t *event)
{
    (void)event;
    const bool paired = has_backend_credentials();
    if (!connection_scr) {
        lv_obj_t *body = NULL;
        connection_scr = create_subpage_shell("设备配对", &body, back_to_index,
                                              paired ? refresh_online_devices_clicked : NULL);
        lv_obj_t *group = make_settings_card(body);
        const char *device_id = purin_device_id();
        const char *server = terminal_server_url();
        make_info_row(group, "设备名称", PURIN_DEVICE_NAME, NULL);
        add_inset_divider(group);
        make_info_row(group, "设备平台", "ESP32-S3", NULL);
        add_inset_divider(group);
        make_info_row(group, "设备 ID", device_id[0] ? device_id : "未生成", NULL);
        add_inset_divider(group);
        make_info_row(group, "服务地址", server[0] ? server : "未配置", NULL);

        lv_obj_t *actions = lv_obj_create(body);
        eink_style_clean_box(actions);
        lv_obj_remove_flag(actions, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(actions, LV_PCT(100), 48);
        lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(actions, 10, LV_PART_MAIN);

        lv_obj_t *scan_button = eink_outline_button(actions, "扫码配对", eink_body_font(), 48,
                                                    open_qr_pair_page, NULL);
        lv_obj_set_flex_grow(scan_button, 1);
        lv_obj_t *link_button = eink_outline_button(actions, "输入链接", eink_body_font(), 48,
                                                    open_pair_page, NULL);
        lv_obj_set_flex_grow(link_button, 1);

        if (paired) {
            online_devices_title = eink_label(body, "在线设备", eink_body_font(), LV_TEXT_ALIGN_LEFT);
            online_devices_card = make_settings_card(body);
            make_info_row(online_devices_card, "状态", "正在读取…", NULL);
        }
    }
    lv_screen_load(connection_scr);
    if (paired) {
        request_online_devices_refresh(!rendered_online_valid);
    }
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

static lv_obj_t *make_settings_switch(lv_obj_t *parent)
{
    lv_obj_t *toggle = lv_switch_create(parent);
    lv_obj_set_size(toggle, 68, 36);
    lv_obj_set_style_anim_duration(toggle, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    return toggle;
}

static void change_connector(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != lv_event_get_current_target_obj(event)) return;
    device_app_t app = (device_app_t)(uintptr_t)lv_event_get_user_data(event);
    esp_err_t err = device_apps_toggle(app);
    bool enabled = (device_apps_mask() & (1u << app)) != 0;
    lv_obj_set_state(connector_switches[app], LV_STATE_CHECKED, enabled);
    const char *message = "";
    if (err != ESP_OK) message = "开关保存失败，请重试";
    else if (enabled && !terminal_tools_enabled()) message = "远程调用需配置 HTTPS 服务与设备凭据";
    lv_label_set_text_static(connector_note, message);
    lv_obj_set_flag(connector_note, LV_OBJ_FLAG_HIDDEN, !*message);
}

static void open_connectors_page(lv_event_t *event)
{
    (void)event;
    if (!connectors_scr) {
        lv_obj_t *body = NULL;
        connectors_scr = create_subpage_shell("连接器", &body, back_to_index, NULL);
        lv_obj_t *group = make_settings_card(body);
        for (unsigned i = 0; i < APP_COUNT; ++i) {
            if (i) add_inset_divider(group);
            lv_obj_t *row = make_info_row(group, app_titles[i], NULL, NULL);
            lv_obj_set_height(row, 80);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, change_connector, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t *toggle = make_settings_switch(row);
            connector_switches[i] = toggle;
            lv_obj_add_event_cb(toggle, change_connector, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)i);
        }
        connector_note = eink_label(body, "", eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(connector_note, LV_PCT(100));
    }
    unsigned mask = device_apps_mask();
    for (unsigned i = 0; i < APP_COUNT; ++i)
        lv_obj_set_state(connector_switches[i], LV_STATE_CHECKED, (mask & (1u << i)) != 0);
    lv_obj_add_flag(connector_note, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(connectors_scr);
}

static void open_appearance_page(lv_event_t *event)
{
    (void)event;
    if (!appearance_scr) {
        lv_obj_t *body = NULL;
        appearance_scr = create_subpage_shell("外观", &body, back_to_index, trigger_full_refresh);

        lv_obj_t *group = make_settings_card(body);

        make_info_row(group, "屏幕类型", "480×800 墨水屏", NULL);
        add_inset_divider(group);
        make_info_row(group, "色彩模式", "1-bit 黑白纸感", NULL);
        add_inset_divider(group);
        make_info_row(group, "刷新策略", "局刷 / 10 帧全刷", NULL);
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

static lv_obj_t *recording_upload_option(lv_obj_t *parent, const char *label, bool automatic)
{
    lv_obj_t *option = lv_checkbox_create(parent);
    eink_style_clean_box(option);
    // Apply selection only after the mutually exclusive preference has been saved.
    lv_obj_remove_flag(option, LV_OBJ_FLAG_CHECKABLE);
    lv_checkbox_set_text_static(option, label);
    lv_obj_set_style_text_font(option, eink_small_font(), 0);
    lv_obj_set_style_text_color(option, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(option, 9, 0);
    lv_obj_set_style_pad_hor(option, 4, 0);
    lv_obj_set_style_pad_column(option, 4, 0);
    lv_obj_set_style_transform_width(option, -4, LV_PART_INDICATOR);
    lv_obj_set_style_transform_height(option, -4, LV_PART_INDICATOR);
    lv_obj_set_style_radius(option, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(option, 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(option, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(option, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(option, lv_color_black(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(option, select_recording_upload, LV_EVENT_CLICKED, (void *)(uintptr_t)automatic);
    return option;
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
        recording_manual_button = recording_upload_option(choices, "手动上传", false);
        recording_auto_button = recording_upload_option(choices, "自动上传", true);
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
        lv_obj_t *toggle = make_settings_switch(progress);
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

static lv_obj_t *index_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *heading = eink_label(parent, title, eink_small_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_pad_left(heading, 12, 0);
    lv_obj_t *group = make_settings_card(parent);
    lv_obj_set_style_border_width(group, 1, 0);
    lv_obj_set_style_pad_all(group, 3, 0);
    lv_obj_set_style_margin_bottom(group, 8, 0);
    return group;
}

static lv_obj_t *index_entry(lv_obj_t *group, const char *symbol, const char *title,
                             const char *summary, lv_event_cb_t open)
{
    if (lv_obj_get_child_count(group)) add_inset_divider(group);
    lv_obj_t *row = make_navigation_row(group);
    lv_obj_set_height(row, 84);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_t *icon = eink_label(row, symbol, &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(icon, 36);

    lv_obj_t *copy = lv_obj_create(row);
    eink_style_clean_box(copy);
    lv_obj_remove_flag(copy, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(copy, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(copy, 1);
    lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(copy, 3, 0);
    eink_label(copy, title, eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_t *detail = eink_label(copy, summary, eink_small_font(), LV_TEXT_ALIGN_LEFT);
    eink_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_event_cb(row, open, LV_EVENT_CLICKED, NULL);
    return detail;
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

    lv_obj_set_style_pad_ver(body, 16, 0);
    lv_obj_set_style_pad_row(body, 10, 0);

    lv_obj_t *device = index_section(body, "设备与采集");
    index_entry(device, LV_SYMBOL_USB, "设备配对",
                has_backend_credentials() ? "已配对" : "尚未配对", open_connection_page);
    idx_network_val = index_entry(device, LV_SYMBOL_WIFI, "网络", "", open_network_page);
    idx_rec_val = index_entry(device, LV_SYMBOL_UPLOAD, "信息采集", "", open_collection_page);

    lv_obj_t *extensions = index_section(body, "能力扩展");
    idx_connectors_val = index_entry(extensions, LV_SYMBOL_SHUFFLE, "连接器", "", open_connectors_page);

    lv_obj_t *general = index_section(body, "通用");
    index_entry(general, LV_SYMBOL_EYE_OPEN, "外观", "墨水屏与刷新", open_appearance_page);
    index_entry(general, LV_SYMBOL_SD_CARD, "存储", "内置闪存与 SD 卡", open_storage_page);
}

void settings_screen_refresh_state(void)
{
    const char *rec = recorder_label_zh();

    refresh_network_state();
    if (idx_rec_val) lv_label_set_text_fmt(idx_rec_val, "录音状态  %s", rec);
    if (idx_connectors_val) {
        unsigned mask = device_apps_mask(), enabled = 0;
        for (unsigned i = 0; i < APP_COUNT; ++i) enabled += (mask >> i) & 1u;
        lv_label_set_text_fmt(idx_connectors_val, "已开启 %u / %u 项", enabled, (unsigned)APP_COUNT);
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
    atomic_store(&qr_scan_cancel, true);
    if (wifi_password && active == wifi_password_scr) lv_textarea_set_text(wifi_password, "");
    if (home_ref &&
        (active == index_scr || active == connection_scr || active == pair_scr ||
         active == qr_pair_scr || active == network_scr || active == wifi_scan_scr ||
         active == wifi_password_scr || active == connectors_scr ||
         active == appearance_scr || active == collection_scr || active == storage_scr)) {
        lv_screen_load(home_ref);
    }
}
