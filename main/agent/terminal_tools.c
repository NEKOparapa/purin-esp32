#include "terminal_tools.h"
#include "terminal_http.h"
#include "terminal_network.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#ifndef WINTERMUTE_DEVICE_STATUS_ENABLED
#define WINTERMUTE_DEVICE_STATUS_ENABLED 0
#endif

typedef struct {
    uint8_t binding[32];
    char id[80];
    char claim_token[80];
    char output[256];
    bool success;
} status_receipt_t;

bool terminal_tools_enabled(void)
{
    return WINTERMUTE_DEVICE_STATUS_ENABLED && terminal_configured() &&
           strncmp(WINTERMUTE_SERVER_URL, "https://", 8) == 0;
}

static cJSON *post(const char *path, const cJSON *body, int *code)
{
    char message[80];
    return terminal_http_json_bounded(message, sizeof(message), HTTP_METHOD_POST, path,
                                      body, 200, 4096, code);
}

static bool url_segment(const char *s, size_t limit)
{
    return s && *s && strlen(s) < limit &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(s);
}

static bool claim(status_receipt_t *receipt)
{
    cJSON *body = cJSON_CreateObject();
    const char *names[] = {"device_status"};
    cJSON *tools = cJSON_CreateStringArray(names, 1);
    if (!body || !tools || !cJSON_AddItemToObject(body, "tools", tools)) {
        cJSON_Delete(tools);
        cJSON_Delete(body);
        return false;
    }
    cJSON *res = post("/v1/terminal-tools/claim", body, NULL);
    cJSON_Delete(body);
    cJSON *call = cJSON_GetObjectItemCaseSensitive(res, "call");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "id"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "claim_token"));
    if (!url_segment(id, sizeof(receipt->id)) || !url_segment(token, sizeof(receipt->claim_token))) {
        cJSON_Delete(res);
        return false;
    }
    strlcpy(receipt->id, id, sizeof(receipt->id));
    strlcpy(receipt->claim_token, token, sizeof(receipt->claim_token));
    const char *tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "tool"));
    cJSON *args = cJSON_GetObjectItemCaseSensitive(call, "arguments");
    receipt->success = tool && strcmp(tool, "device_status") == 0 && cJSON_IsObject(args) && !args->child;
    cJSON_Delete(res);
    if (receipt->success) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        snprintf(receipt->output, sizeof(receipt->output),
                 "{\"board\":\"Metalio E-Ink 4\",\"chip\":\"%s\",\"cores\":%u,"
                 "\"uptime_seconds\":%" PRId64 ",\"free_heap_bytes\":%" PRIu32 "}",
                 CONFIG_IDF_TARGET, (unsigned)chip.cores, esp_timer_get_time() / 1000000,
                 esp_get_free_heap_size());
    } else {
        strlcpy(receipt->output, "Expected device_status with empty arguments", sizeof(receipt->output));
    }
    return true;
}

static bool deliver(const status_receipt_t *receipt)
{
    char path[128];
    snprintf(path, sizeof(path), "/v1/terminal-tools/%s/result", receipt->id);
    cJSON *body = cJSON_CreateObject();
    cJSON *ack = NULL;
    int code = 0;
    if (body && cJSON_AddStringToObject(body, "claim_token", receipt->claim_token) &&
        cJSON_AddStringToObject(body, "status", receipt->success ? "succeeded" : "failed") &&
        cJSON_AddStringToObject(body, "stdout", receipt->success ? receipt->output : "") &&
        cJSON_AddStringToObject(body, "stderr", receipt->success ? "" : receipt->output) &&
        cJSON_AddBoolToObject(body, "truncated", false) &&
        (receipt->success ? cJSON_AddNumberToObject(body, "exit_code", 0)
                          : cJSON_AddNullToObject(body, "exit_code"))) {
        ack = post(path, body, &code);
    }
    bool done = ack || code == 404 || code == 409 || code == 410;
    cJSON_Delete(ack);
    cJSON_Delete(body);
    return done;
}

void terminal_tools_poll(nvs_handle_t journal)
{
    // Never send a cached result/token after the board has been reprovisioned.
    static const char identity[] = WINTERMUTE_SERVER_URL "\0" WINTERMUTE_TERMINAL_ID "\0" WINTERMUTE_TERMINAL_TOKEN;
    uint8_t binding[32];
    if (mbedtls_sha256((const uint8_t *)identity, sizeof(identity), binding, 0) != 0) return;
    status_receipt_t saved = {0};
    size_t size = sizeof(saved);
    esp_err_t rc = nvs_get_blob(journal, "tool_receipt", &saved, &size);
    if (rc != ESP_ERR_NVS_NOT_FOUND && (rc != ESP_OK || size != sizeof(saved))) return;
    bool pending = rc == ESP_OK && memcmp(saved.binding, binding, sizeof(binding)) == 0;
    if (rc == ESP_OK && !pending) {
        if (nvs_erase_key(journal, "tool_receipt") != ESP_OK || nvs_commit(journal) != ESP_OK) return;
    }
    if (terminal_network_acquire() != ESP_OK) return;
    if (!pending && claim(&saved)) {
        memcpy(saved.binding, binding, sizeof(binding));
        pending = nvs_set_blob(journal, "tool_receipt", &saved, sizeof(saved)) == ESP_OK &&
                  nvs_commit(journal) == ESP_OK;
    }
    if (pending && deliver(&saved) && nvs_erase_key(journal, "tool_receipt") == ESP_OK) {
        nvs_commit(journal);
    }
    terminal_network_release();
}
