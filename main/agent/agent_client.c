#include "agent_client.h"
#include "terminal_network.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "terminal_http.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
static bool busy;
static agent_result_t no_memory = {.status = "Out of memory. Tap Retry"};
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

static bool extract_replies(const cJSON *json, agent_result_t *result)
{
    const cJSON *outputs = cJSON_GetObjectItemCaseSensitive(json, "outputs");
    if (!cJSON_IsArray(outputs)) return false;
    agent_reply_t **tail = &result->replies;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, outputs) {
        const char *id = field(item, "output_id");
        const char *text = field(item, "text");
        const char *status = field(item, "status");
        const char *layer = field(item, "layer");
        if (!valid_id(id) || !text || !status || !layer) return false;
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

static void inbox(agent_result_t *result)
{
    cJSON *json = request(result, HTTP_METHOD_GET, "/v1/outputs?limit=1", NULL, 200);
    if (!json) return;
    const char *label = "Cannot read inbox";
    if (extract_replies(json, result)) label = result->replies ? "Unread reply" : "Inbox is clear";
    strlcpy(result->status, label, sizeof(result->status));
    cJSON_Delete(json);
}

static void sync_turn(agent_result_t *result, turn_t *turn)
{
    if (!turn->id[0]) {
        inbox(result);
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
            strlcpy(result->status, "Cannot save job. Tap Retry", sizeof(result->status));
            return;
        }
        result->sent = true;
        result->poll_job = true;
        strlcpy(result->status, "Sent. Waiting for Agent", sizeof(result->status));
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/v1/jobs/%s", turn->job);
    cJSON *job = request(result, HTTP_METHOD_GET, path, NULL, 200);
    if (!job) return;
    const char *status = field(job, "status");
    if (!status || !extract_replies(job, result)) {
        strlcpy(result->status, "Cannot read job. Tap Retry", sizeof(result->status));
    } else if (!strcmp(status, "queued") || !strcmp(status, "running")) {
        result->poll_job = true;
        strlcpy(result->status, "Waiting for Agent", sizeof(result->status));
    } else if (!strcmp(status, "completed")) {
        strlcpy(result->status, result->replies ? "Reply received" : "Finished without text", sizeof(result->status));
    } else {
        snprintf(result->status, sizeof(result->status), "Job %s", status);
    }
    cJSON_Delete(job);
}

static void share_state(agent_result_t *result, const char *state)
{
    capture_t capture = {0};
    if (!load("capture", &capture, sizeof(capture))) return;
    if (!capture.id[0] || strcmp(state, capture.state)) {
        make_id(capture.id);
        strlcpy(capture.state, state, sizeof(capture.state));
        if (!store("capture", &capture, sizeof(capture))) return;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *receipt = NULL;
    if (cJSON_AddStringToObject(body, "event_id", capture.id) &&
        cJSON_AddStringToObject(body, "state_key", "recorder.state") &&
        cJSON_AddStringToObject(body, "value", capture.state)) {
        receipt = request(result, HTTP_METHOD_POST, "/v1/perceptions", body, 202);
    }
    cJSON_Delete(body);
    if (!receipt) return;
    if (field(receipt, "event_id") && nvs_erase_key(journal, "capture") == ESP_OK && nvs_commit(journal) == ESP_OK) {
        snprintf(result->status, sizeof(result->status), "Shared recorder state: %s", capture.state);
    }
    cJSON_Delete(receipt);
}

static void execute(const command_t *cmd, agent_result_t *result)
{
    strlcpy(result->status, "Device storage or memory unavailable", sizeof(result->status));
    if (!terminal_configured()) {
        strlcpy(result->status, "Set Wi-Fi, server and token in device config", sizeof(result->status));
        return;
    }
    turn_t turn = {0};
    if (!load("turn", &turn, sizeof(turn))) return;
    if (cmd->action == AGENT_SEND) {
        if (turn.id[0] && !turn.job[0] && strcmp(turn.question, cmd->value)) {
            strlcpy(result->status, "Retry your saved message first", sizeof(result->status));
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
        strlcpy(result->status, "Wi-Fi unavailable. Try again", sizeof(result->status));
        return;
    }
    switch (cmd->action) {
    case AGENT_SEND:
    case AGENT_SYNC:
        sync_turn(result, &turn);
        break;
    case AGENT_INBOX:
        inbox(result);
        break;
    case AGENT_READ:
        if (valid_id(cmd->value)) {
            char path[128];
            snprintf(path, sizeof(path), "/v1/outputs/%.*s/ack", AGENT_ID_BYTES-1, cmd->value);
            cJSON *receipt = request(result, HTTP_METHOD_POST, path, NULL, 200);
            if (receipt) {
                result->acknowledged = true;
                cJSON_Delete(receipt);
                inbox(result);
            }
        }
        break;
    case AGENT_CAPTURE:
        share_state(result, cmd->value);
        break;
    }
    terminal_network_release();
}

static void worker(void *arg)
{
    (void)arg;
    command_t cmd;
    for (;;) {
        xQueueReceive(requests, &cmd, portMAX_DELAY);
        agent_result_t *result = calloc(1, sizeof(*result));
        if (result) execute(&cmd, result);
        xQueueSend(responses, &result, portMAX_DELAY);
    }
}

esp_err_t agent_client_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_OK) error = nvs_open("wm_chat", NVS_READWRITE, &journal);
    if (error != ESP_OK) return error;
    requests = xQueueCreate(1, sizeof(command_t));
    responses = xQueueCreate(1, sizeof(agent_result_t *));
    if (requests && responses && xTaskCreate(worker, "agent", 10240, NULL, 1, NULL) == pdPASS) return ESP_OK;
    if (requests) vQueueDelete(requests);
    if (responses) vQueueDelete(responses);
    requests = NULL;
    responses = NULL;
    nvs_close(journal);
    return ESP_ERR_NO_MEM;
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
