#include "agent_client.h"
#include "terminal_network.h"
#include "terminal_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "terminal_http.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef struct {
    agent_action_t action;
    char value[AGENT_INPUT_BYTES];
} command_t;

typedef struct {
    char id[33];
    char question[AGENT_INPUT_BYTES];
    char job[AGENT_ID_BYTES];
} turn_t;

typedef struct {
    char id[33];
    char state[32];
} capture_t;

static QueueHandle_t requests;
static QueueHandle_t responses;
static SemaphoreHandle_t state_lock;
static char desired_recorder_state[32];
static char reported_recorder_state[32];
static bool recorder_state_dirty;
static bool busy;
static agent_result_t no_memory = {.status = "Out of memory. Tap 刷新"};
static nvs_handle_t journal;

static void make_id(char id[33])
{
    uint32_t rnd[4];
    esp_fill_random(rnd, sizeof(rnd));
    snprintf(id, 33, "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
             rnd[0], rnd[1], rnd[2], rnd[3]);
}

static bool store(const char *key, const void *data, size_t size)
{
    return nvs_set_blob(journal, key, data, size) == ESP_OK && nvs_commit(journal) == ESP_OK;
}

static bool load(const char *key, void *data, size_t size)
{
    size_t actual = size;
    esp_err_t error = nvs_get_blob(journal, key, data, &actual);
    return error == ESP_ERR_NVS_NOT_FOUND || (error == ESP_OK && actual == size);
}

static cJSON *request(agent_result_t *result, esp_http_client_method_t method,
                      const char *path, const cJSON *body, int expected)
{
    return terminal_http_json(result->status, sizeof(result->status), method, path, body, expected);
}

static const char *field(const cJSON *object, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

// IDs returned by Wintermute are opaque URL path segments.
static bool valid_id(const char *id)
{
    return id && *id && strlen(id) < AGENT_ID_BYTES &&
           strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(id);
}

static bool has_reply_id(const agent_result_t *result, const char *id)
{
    for (const agent_reply_t *r = result->replies; r; r = r->next) {
        if (!strcmp(r->id, id)) return true;
    }
    return false;
}

static bool extract_replies(const cJSON *json, agent_result_t *result)
{
    const cJSON *outputs = cJSON_GetObjectItemCaseSensitive(json, "outputs");
    if (!cJSON_IsArray(outputs)) return false;
    agent_reply_t **tail = &result->replies;
    while (*tail) tail = &(*tail)->next;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, outputs) {
        const char *id = field(item, "output_id");
        const char *text = field(item, "text");
        const char *status = field(item, "status");
        const char *layer = field(item, "layer");
        if (!valid_id(id) || !text || !status || !layer) return false;
        if (has_reply_id(result, id)) continue;
        agent_reply_t *reply = calloc(1, sizeof(*reply));
        if (!reply) return false;
        *tail = reply;
        reply->text = strdup(text);
        if (!reply->text) return false;
        strlcpy(reply->id, id, sizeof(reply->id));
        reply->unread = strcmp(status, "pending") == 0;
        reply->notification = !strcmp(layer, "notification");
        tail = &reply->next;
    }
    return true;
}

static void ack_unread_replies(agent_result_t *result)
{
    char ignored_status[64];
    for (agent_reply_t *r = result->replies; r; r = r->next) {
        if (!r->unread || !valid_id(r->id)) continue;
        char path[128];
        snprintf(path, sizeof(path), "/v1/outputs/%.*s/ack", AGENT_ID_BYTES - 1, r->id);
        cJSON *receipt = terminal_http_json(ignored_status, sizeof(ignored_status),
                                            HTTP_METHOD_POST, path, NULL, 200);
        if (receipt) {
            r->unread = false;
            result->acknowledged = true;
            cJSON_Delete(receipt);
        }
    }
}

static void fetch_inbox(agent_result_t *result, bool update_status)
{
    char tmp_status[112] = {0};
    cJSON *json = terminal_http_json(
        update_status ? result->status : tmp_status,
        update_status ? sizeof(result->status) : sizeof(tmp_status),
        HTTP_METHOD_GET, "/v1/outputs?limit=5", NULL, 200);
    if (!json) return;
    bool ok = extract_replies(json, result);
    cJSON_Delete(json);
    if (ok) {
        ack_unread_replies(result);
        if (update_status) {
            strlcpy(result->status, result->replies ? "已同步最新回复" : "已是最新状态",
                    sizeof(result->status));
        }
    } else if (update_status) {
        strlcpy(result->status, "无法读取最新回复", sizeof(result->status));
    }
}

static void sync_turn(agent_result_t *result, turn_t *turn)
{
    if (!turn->id[0]) {
        fetch_inbox(result, true);
        return;
    }
    strlcpy(result->message_id, turn->id, sizeof(result->message_id));
    strlcpy(result->question, turn->question, sizeof(result->question));
    if (!turn->job[0]) {
        cJSON *body = cJSON_CreateObject();
        cJSON *receipt = NULL;
        if (cJSON_AddStringToObject(body, "message_id", turn->id) &&
            cJSON_AddStringToObject(body, "text", turn->question)) {
            receipt = request(result, HTTP_METHOD_POST, "/v1/messages", body, 202);
        }
        cJSON_Delete(body);
        if (!receipt) return;
        const char *id = field(receipt, "job_id");
        bool valid = valid_id(id);
        if (valid) strlcpy(turn->job, id, sizeof(turn->job));
        cJSON_Delete(receipt);
        if (!valid || !store("turn", turn, sizeof(*turn))) {
            strlcpy(result->status, "保存任务失败，请点击刷新重试", sizeof(result->status));
            return;
        }
        result->sent = true;
        result->poll_job = true;
        strlcpy(result->status, "已发送，等待 Agent 回复...", sizeof(result->status));
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/v1/jobs/%s", turn->job);
    cJSON *job = request(result, HTTP_METHOD_GET, path, NULL, 200);
    if (!job) return;
    const char *status = field(job, "status");
    if (!status || !extract_replies(job, result)) {
        strlcpy(result->status, "读取任务失败，请点击刷新", sizeof(result->status));
    } else if (!strcmp(status, "queued") || !strcmp(status, "running")) {
        result->poll_job = true;
        strlcpy(result->status, "Agent 正在思考...", sizeof(result->status));
    } else if (!strcmp(status, "completed")) {
        ack_unread_replies(result);
        // Also pull any other pending inbox outputs in the same refresh cycle.
        fetch_inbox(result, false);
        strlcpy(result->status, result->replies ? "已收到回复" : "已完成", sizeof(result->status));
    } else {
        snprintf(result->status, sizeof(result->status), "任务状态：%s", status);
        fetch_inbox(result, false);
    }
    cJSON_Delete(job);
}

static bool share_state_silent(const char *state)
{
    capture_t capture = {0};
    if (!load("capture", &capture, sizeof(capture))) return false;
    if (!capture.id[0] || strcmp(state, capture.state)) {
        make_id(capture.id);
        strlcpy(capture.state, state, sizeof(capture.state));
        if (!store("capture", &capture, sizeof(capture))) return false;
    }
    char ignored[64];
    cJSON *body = cJSON_CreateObject();
    cJSON *receipt = NULL;
    if (cJSON_AddStringToObject(body, "event_id", capture.id) &&
        cJSON_AddStringToObject(body, "state_key", "recorder.state") &&
        cJSON_AddStringToObject(body, "value", capture.state)) {
        receipt = terminal_http_json(ignored, sizeof(ignored), HTTP_METHOD_POST,
                                     "/v1/perceptions", body, 202);
    }
    cJSON_Delete(body);
    if (!receipt) return false;
    bool ok = field(receipt, "event_id") != NULL;
    if (ok && nvs_erase_key(journal, "capture") == ESP_OK) {
        (void)nvs_commit(journal);
    }
    cJSON_Delete(receipt);
    return ok;
}

static void flush_recorder_state(void)
{
    if (!state_lock || !terminal_configured()) return;
    char state[32] = {0};
    bool dirty = false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (recorder_state_dirty && desired_recorder_state[0] &&
        strcmp(desired_recorder_state, reported_recorder_state) != 0) {
        strlcpy(state, desired_recorder_state, sizeof(state));
        dirty = true;
    } else {
        recorder_state_dirty = false;
    }
    xSemaphoreGive(state_lock);

    if (!dirty) return;
    if (terminal_network_acquire() != ESP_OK) return;
    bool ok = share_state_silent(state);
    terminal_network_release();
    if (ok) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        strlcpy(reported_recorder_state, state, sizeof(reported_recorder_state));
        if (strcmp(desired_recorder_state, reported_recorder_state) == 0) {
            recorder_state_dirty = false;
        }
        xSemaphoreGive(state_lock);
    }
}

static void execute(const command_t *cmd, agent_result_t *result)
{
    strlcpy(result->status, "设备存储不可用", sizeof(result->status));
    if (!terminal_configured()) {
        strlcpy(result->status, terminal_network_mode() == TERMINAL_NETWORK_OFF
            ? "请先在网络设置中开启连接" : "请检查固件中的网络和服务配置", sizeof(result->status));
        return;
    }
    turn_t turn = {0};
    if (!load("turn", &turn, sizeof(turn))) return;
    if (cmd->action == AGENT_SEND) {
        if (turn.id[0] && !turn.job[0] && strcmp(turn.question, cmd->value)) {
            strlcpy(result->status, "请先点击刷新重发上一条消息", sizeof(result->status));
            return;
        }
        if (!turn.id[0] || turn.job[0]) {
            memset(&turn, 0, sizeof(turn));
            make_id(turn.id);
            strlcpy(turn.question, cmd->value, sizeof(turn.question));
            if (!store("turn", &turn, sizeof(turn))) return;
        }
    }
    esp_err_t error = terminal_network_acquire();
    if (error != ESP_OK) {
        strlcpy(result->status, "无法联网，请在网络设置中查看状态", sizeof(result->status));
        return;
    }
    switch (cmd->action) {
    case AGENT_SEND:
    case AGENT_SYNC:
        sync_turn(result, &turn);
        break;
    }
    terminal_network_release();
}

static void worker(void *arg)
{
    (void)arg;
    command_t cmd;
    const bool accept_tools = terminal_tools_enabled();
    const TickType_t tool_period = pdMS_TO_TICKS(6000);
    const TickType_t state_period = pdMS_TO_TICKS(1500);
    TickType_t last_tool_poll = xTaskGetTickCount() - tool_period;
    for (;;) {
        bool has_state = false;
        if (state_lock) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            has_state = recorder_state_dirty;
            xSemaphoreGive(state_lock);
        }

        TickType_t wait = portMAX_DELAY;
        if (accept_tools) {
            TickType_t elapsed = xTaskGetTickCount() - last_tool_poll;
            wait = elapsed < tool_period ? tool_period - elapsed : 0;
        }
        if (has_state && (wait == portMAX_DELAY || state_period < wait)) {
            wait = state_period;
        }

        if (xQueueReceive(requests, &cmd, wait) == pdTRUE) {
            agent_result_t *result = calloc(1, sizeof(*result));
            if (result) execute(&cmd, result);
            xQueueSend(responses, &result, portMAX_DELAY);
        }
        flush_recorder_state();
        if (accept_tools && (xTaskGetTickCount() - last_tool_poll) >= tool_period) {
            terminal_tools_poll(journal);
            last_tool_poll = xTaskGetTickCount();
        }
    }
}

esp_err_t agent_client_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_OK) error = nvs_open("wm_chat", NVS_READWRITE, &journal);
    if (error != ESP_OK) return error;
    requests = xQueueCreate(1, sizeof(command_t));
    responses = xQueueCreate(1, sizeof(agent_result_t *));
    state_lock = xSemaphoreCreateMutex();
    if (requests && responses && state_lock &&
        xTaskCreate(worker, "agent", 10240, NULL, 1, NULL) == pdPASS) {
        return ESP_OK;
    }
    if (requests) vQueueDelete(requests);
    if (responses) vQueueDelete(responses);
    if (state_lock) vSemaphoreDelete(state_lock);
    requests = NULL;
    responses = NULL;
    state_lock = NULL;
    nvs_close(journal);
    return ESP_ERR_NO_MEM;
}

void agent_client_report_recorder_state(const char *state)
{
    if (!state_lock || !state || !*state) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (strcmp(desired_recorder_state, state) != 0) {
        strlcpy(desired_recorder_state, state, sizeof(desired_recorder_state));
        recorder_state_dirty = strcmp(desired_recorder_state, reported_recorder_state) != 0;
    }
    xSemaphoreGive(state_lock);
}

bool agent_client_start(agent_action_t action, const char *value)
{
    if (busy || !requests || (value && strlen(value) >= AGENT_INPUT_BYTES)) return false;
    command_t cmd = {.action = action};
    strlcpy(cmd.value, value ? value : "", sizeof(cmd.value));
    busy = xQueueSend(requests, &cmd, 0) == pdTRUE;
    return busy;
}

agent_result_t *agent_client_take_result(void)
{
    agent_result_t *result = NULL;
    if (responses && xQueueReceive(responses, &result, 0) == pdTRUE) {
        busy = false;
        // Caller needs a completion even under memory pressure.
        if (!result) {
            return &no_memory;
        }
    }
    return result;
}

void agent_result_free(agent_result_t *result)
{
    if (!result || result == &no_memory) return;
    while (result->replies) {
        agent_reply_t *next = result->replies->next;
        free(result->replies->text);
        free(result->replies);
        result->replies = next;
    }
    free(result);
}
