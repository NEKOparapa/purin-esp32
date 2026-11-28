#include "device_apps.h"
#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

const char *const app_ids[] = {"esp_vibration", "esp_player", "esp_card", "esp_timer", "esp_library"};
const char *const app_titles[] = {"震动器", "播放器", "提示卡片", "计时器", "书库管理"};
static nvs_handle_t prefs;
static SemaphoreHandle_t lock;
static atomic_uint enabled;
static prompt_card_t card;
static char timer_label[193];
static int64_t deadline;
static int timer_seconds;
static uint32_t timer_run;
static bool expired;
static desk_notice_t notice;
static int64_t notice_until;
static const int64_t notice_duration_us = 10 * 1000000LL;

esp_err_t device_apps_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) err = nvs_open("pr_apps", NVS_READWRITE, &prefs);
    if (err != ESP_OK) return err;
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    uint32_t mask = 0;
    nvs_get_u32(prefs, "enabled", &mask);
    atomic_store(&enabled, mask & ((1u << APP_COUNT) - 1));
    size_t bytes = sizeof(card);
    if (nvs_get_blob(prefs, "card", &card, &bytes) != ESP_OK || bytes != sizeof(card) ||
        card.id[sizeof(card.id)-1] || card.title[sizeof(card.title)-1] || card.body[sizeof(card.body)-1])
        memset(&card, 0, sizeof(card));
    return ESP_OK;
}

unsigned device_apps_mask(void) { return atomic_load(&enabled); }

esp_err_t device_apps_toggle(device_app_t app)
{
    if (!lock || (unsigned)app >= APP_COUNT) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned next = device_apps_mask() ^ (1u << app);
    esp_err_t err = nvs_set_u32(prefs, "enabled", next);
    if (err == ESP_OK) err = nvs_commit(prefs);
    if (err == ESP_OK) atomic_store(&enabled, next);
    xSemaphoreGive(lock);
    if (err == ESP_OK && !(next & (1u << app))) {
        if (app == APP_PLAYER) player_stop();
        if (app == APP_VIBRATION) board_vibration_stop();
    }
    return err;
}

const char *app_text(const cJSON *args, const char *key, size_t max_chars)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, key));
    if (!s || !*s || strlen(s) > max_chars * 4 || !s[strspn(s, " \t\n\r")]) return NULL;
    size_t chars = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned first = *p++;
        if (++chars > max_chars || (first < 32 && first != '\t' && first != '\n' && first != '\r')) return NULL;
        if (first < 128) continue;
        if (first < 0xc2 || first > 0xf4) return NULL;
        if ((first == 0xe0 && *p < 0xa0) || (first == 0xed && *p >= 0xa0) ||
            (first == 0xf0 && *p < 0x90) || (first == 0xf4 && *p >= 0x90)) return NULL;
        int tail = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        while (tail-- > 0) if ((*p++ & 0xc0) != 0x80) return NULL;
    }
    return s;
}

int app_number(const cJSON *args, const char *key, int min, int max, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!v) return fallback;
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != v->valueint) return -1;
    return v->valueint;
}

cJSON *app_error(char *error, size_t size, const char *message)
{
    strlcpy(error, message, size);
    return NULL;
}

cJSON *app_result(const char *status)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj && !cJSON_AddStringToObject(obj, "status", status)) { cJSON_Delete(obj); return NULL; }
    return obj;
}

static int seconds_left(void)
{
    int64_t us = deadline ? deadline - esp_timer_get_time() : 0;
    return us <= 0 ? 0 : (int)((us + 999999) / 1000000);
}

void device_apps_tick(void)
{
    if (!lock || !xSemaphoreTake(lock, 0)) return;
    bool ring = deadline && !seconds_left();
    if (ring) {
        deadline = 0;
        expired = true;
        if (notice.kind == DESK_TIMER) {
            notice.kind = DESK_TIMER_END;
            notice_until = esp_timer_get_time() + notice_duration_us;
        }
    }
    xSemaphoreGive(lock);
    if (ring && (device_apps_mask() & (1u << APP_VIBRATION))) board_vibration_pulse(250);
}

void device_apps_snapshot(desk_state_t *out)
{
    if (!lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (notice_until && esp_timer_get_time() >= notice_until) {
        notice = (desk_notice_t){0};
        notice_until = 0;
    }
    out->notice = notice;
    out->card = card;
    strlcpy(out->timer_label, timer_label, sizeof(out->timer_label));
    out->seconds_left = seconds_left();
    out->timer_seconds = timer_seconds;
    out->timer_run = timer_run;
    out->expired = expired;
    xSemaphoreGive(lock);
}

static bool save_card(const prompt_card_t *value)
{
    if (nvs_set_blob(prefs, "card", value, sizeof(*value)) != ESP_OK || nvs_commit(prefs) != ESP_OK)
        return false;
    card = *value;
    return true;
}

bool device_apps_acknowledge(const char *id)
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    prompt_card_t next = card;
    next.acknowledged = true;
    bool ok = *id && !strcmp(next.id, id) && (card.acknowledged || save_card(&next));
    if (ok && notice.kind == DESK_CARD) notice = (desk_notice_t){0};
    xSemaphoreGive(lock);
    return ok;
}

static cJSON *card_call(const char *action, const cJSON *args, const char *id, char *error, size_t size)
{
    if (!strcmp(action, "show")) {
        const char *title = app_text(args, "title", 48), *body = app_text(args, "body", 240);
        if (!title || !body || !id || strlen(id) >= sizeof(card.id))
            return app_error(error, size, "Invalid card title, body or ID");
        prompt_card_t next = {0};
        strlcpy(next.id, id, sizeof(next.id));
        strlcpy(next.title, title, sizeof(next.title));
        strlcpy(next.body, body, sizeof(next.body));
        if (!save_card(&next)) return app_error(error, size, "Cannot save card");
        notice = (desk_notice_t){.kind = DESK_CARD, .app = APP_CARD};
        notice_until = 0;
    } else if (strcmp(action, "status")) return app_error(error, size, "Unknown card action");
    cJSON *out = app_result(!*card.id ? "empty" : card.acknowledged ? "acknowledged" : "pending");
    if (out && *card.id) {
        cJSON_AddStringToObject(out, "id", card.id);
        cJSON_AddStringToObject(out, "title", card.title);
    }
    return out;
}

static void stop_timer_locked(void)
{
    expired = false;
    deadline = 0;
    timer_label[0] = 0;
    timer_seconds = 0;
    if (notice.kind == DESK_TIMER || notice.kind == DESK_TIMER_END) {
        notice = (desk_notice_t){0};
        notice_until = 0;
    }
}

void device_apps_stop_home_timer(uint32_t displayed_run)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (notice.kind == DESK_TIMER && timer_run == displayed_run) stop_timer_locked();
    xSemaphoreGive(lock);
}

static cJSON *timer_call(const char *action, const cJSON *args, char *error, size_t size)
{
    if (!strcmp(action, "start")) {
        int seconds = app_number(args, "seconds", 1, 86400, -1);
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "label"));
        if (seconds < 1 || (label && *label && !app_text(args, "label", 48)))
            return app_error(error, size, "Invalid timer duration or label");
        timer_seconds = seconds;
        ++timer_run;
        deadline = esp_timer_get_time() + (int64_t)seconds * 1000000;
        expired = false;
        notice = (desk_notice_t){.kind = DESK_TIMER, .app = APP_TIMER};
        notice_until = 0;
        strlcpy(timer_label, label && *label ? label : "倒计时", sizeof(timer_label));
    } else if (!strcmp(action, "stop")) {
        stop_timer_locked();
    } else if (strcmp(action, "status")) return app_error(error, size, "Unknown timer action");
    int remaining = seconds_left();
    cJSON *out = app_result(remaining ? "running" : (expired || deadline) ? "finished" : "idle");
    if (out) {
        cJSON_AddNumberToObject(out, "remaining_seconds", remaining);
        cJSON_AddStringToObject(out, "label", timer_label);
    }
    return out;
}

static cJSON *run_action(device_app_t app, const char *action, const cJSON *args,
                         const char *id, char *error, size_t size)
{
    if (app == APP_LIBRARY) return library_call(args, error, size);
    if (app == APP_PLAYER) return player_call(args, error, size);
    if (app == APP_VIBRATION) {
        esp_err_t err = ESP_OK;
        if (!strcmp(action, "pulse")) {
            int ms = app_number(args, "duration_ms", 1, 1000, 100);
            if (ms < 1) return app_error(error, size, "Pulse must be 1..1000 ms");
            err = board_vibration_pulse(ms);
        } else if (!strcmp(action, "stop")) err = board_vibration_stop();
        else if (strcmp(action, "status")) return app_error(error, size, "Unknown vibration action");
        if (err != ESP_OK) return app_error(error, size, esp_err_to_name(err));
        return app_result(board_vibration_is_active() ? "on" : "off");
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *out = app == APP_CARD ? card_call(action, args, id, error, size)
                                 : timer_call(action, args, error, size);
    xSemaphoreGive(lock);
    return out;
}

// The terminal service publishes only the outcome. All display access stays on the UI task.
static void report_action(device_app_t app, const char *action, const cJSON *args, const char *error)
{
    desk_notice_t next = {.app = app};
    const char *title = "", *book_id = "";
    if (!strcmp(action, "status") || !strcmp(action, "list") || !strcmp(action, "read")) return;
    if (error) {
        next.kind = DESK_ERROR;
        title = !strcmp(error, "Audio is in use") ? "音频正被占用" : error;
    } else if (app == APP_LIBRARY && (!strcmp(action, "add") || !strcmp(action, "append"))) {
        next.kind = !strcmp(action, "add") ? DESK_BOOK : DESK_CHAPTER;
        title = app_text(args, "title", 48);
        book_id = app_text(args, "id", 12);
    } else if (app == APP_LIBRARY && !strcmp(action, "delete")) {
        next.kind = DESK_BOOK_DELETED;
    } else if (!strcmp(action, "pulse")) {
        next.kind = DESK_PULSE;
    } else if (!strcmp(action, "play")) {
        next.kind = DESK_PLAYBACK;
        title = app_text(args, "file", 64);
        if (!title) title = "提示音";
    } else if (!strcmp(action, "stop")) {
        next.kind = DESK_STOPPED;
    }
    if (next.kind == DESK_NONE) return;
    if (next.kind == DESK_PULSE)
        snprintf(next.title, sizeof(next.title), "单次短震 %d 毫秒", app_number(args, "duration_ms", 1, 1000, 100));
    else strlcpy(next.title, title, sizeof(next.title));
    strlcpy(next.book_id, book_id, sizeof(next.book_id));
    xSemaphoreTake(lock, portMAX_DELAY);
    notice = next;
    notice_until = esp_timer_get_time() + notice_duration_us;
    xSemaphoreGive(lock);
}

cJSON *device_apps_call(device_app_t app, const cJSON *args, const char *id, char *error, size_t size)
{
    const char *action = app_text(args, "action", 16);
    if (!lock || !action || (unsigned)app >= APP_COUNT)
        return app_error(error, size, "App unavailable or missing action");
    cJSON *out = run_action(app, action, args, id, error, size);
    if (id) report_action(app, action, args, out ? NULL : error);
    return out;
}
