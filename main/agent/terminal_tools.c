#include "terminal_tools.h"
#include "device_apps.h"
#include <stddef.h>
#include "terminal_http.h"
#include "terminal_network.h"
#include "terminal_pairing.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#ifndef PURIN_DEVICE_STATUS_ENABLED
#define PURIN_DEVICE_STATUS_ENABLED 0
#endif

typedef struct {
    uint8_t binding[32];
    char id[80], claim_token[80];
    bool success;
    char output[2304]; // Four 255-byte TXT filenames and display titles, plus the JSON envelope.
} status_receipt_t;

static status_receipt_t receipt; // Only the terminal service accesses the receipt.
static unsigned registered, catalog_enabled;
static bool withdrew;


bool terminal_tools_enabled(void)
{
    return terminal_configured() &&
           strncmp(terminal_server_url(), "https://", 8) == 0;
}

static cJSON *post(const char *path, const cJSON *body, int *code)
{
    char message[80];
    return terminal_http_json_bounded(message, sizeof(message), HTTP_METHOD_POST, path,
                                      body, 200, 48 * 1024, code);
}

static bool url_segment(const char *s, size_t limit)
{
    return s && *s && strlen(s) < limit &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(s);
}

static void sync_catalog(unsigned mask)
{
    for (unsigned i = 0; i < APP_COUNT; i++) {
        unsigned bit = 1u << i;
        if ((registered & bit) && !((catalog_enabled ^ mask) & bit)) continue;
        cJSON *body = cJSON_CreateObject();
        cJSON *reply = NULL;
        if (body && cJSON_AddStringToObject(body, "id", app_ids[i]) &&
            cJSON_AddStringToObject(body, "name", app_titles[i]) &&
            cJSON_AddBoolToObject(body, "enabled", (mask & bit) != 0))
            reply = post("/v1/connectors/device-sync", body, NULL);
        cJSON_Delete(body);
        if (!reply) break;
        cJSON_Delete(reply);
        registered |= bit;
        catalog_enabled = (catalog_enabled & ~bit) | (mask & bit);
    }
}

static cJSON *claim(unsigned mask)
{
    const char *names[APP_COUNT + 1];
    int count = 0;
    if (PURIN_DEVICE_STATUS_ENABLED) names[count++] = "device_status";
    for (unsigned i = 0; i < APP_COUNT; ++i)
        if (mask & (1u << i)) names[count++] = app_ids[i];
    cJSON *body = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateStringArray(names, count);
    if (!body || !tools || !cJSON_AddItemToObject(body, "tools", tools)) {
        cJSON_Delete(tools); cJSON_Delete(body); return NULL;
    }
    cJSON *res = post("/v1/terminal-tools/claim", body, NULL);
    cJSON_Delete(body);
    if (res) withdrew = count == 0;
    return res;
}

static void execute(status_receipt_t *r, const cJSON *call)
{
    const char *tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "tool"));
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(call, "arguments");
    strlcpy(r->output, "Invalid or disabled tool", sizeof(r->output));
    if (!tool || !cJSON_IsObject(args)) return;
    if (PURIN_DEVICE_STATUS_ENABLED && !strcmp(tool, "device_status") && !args->child) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        snprintf(r->output, sizeof(r->output),
                 "{\"board\":\"Metalio E-Ink 4\",\"chip\":\"%s\",\"cores\":%u,"
                 "\"uptime_seconds\":%" PRId64 ",\"free_heap_bytes\":%" PRIu32 "}",
                 CONFIG_IDF_TARGET, (unsigned)chip.cores, esp_timer_get_time() / 1000000,
                 esp_get_free_heap_size());
        r->success = true;
        return;
    }
    for (unsigned i = 0; i < APP_COUNT; ++i) {
        if (strcmp(tool, app_ids[i]) || !(device_apps_mask() & (1u << i))) continue;
        strlcpy(r->output, "Cannot allocate tool result", sizeof(r->output));
        cJSON *out = device_apps_call(i, args, r->id, r->output, sizeof(r->output));
        if (out) {
            r->success = cJSON_PrintPreallocated(out, r->output, sizeof(r->output), false);
            if (!r->success) strlcpy(r->output, "Tool result too large", sizeof(r->output));
            cJSON_Delete(out);
        }
        break;
    }
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

static bool save_receipt(nvs_handle_t journal)
{
    size_t length = offsetof(status_receipt_t, output) + strlen(receipt.output) + 1;
    return nvs_set_blob(journal, "app_receipt", &receipt, length) == ESP_OK &&
           nvs_commit(journal) == ESP_OK;
}

bool terminal_tools_poll(nvs_handle_t journal)
{
    // Never send a cached result/token after the board has been reprovisioned.
    char identity[sizeof(PURIN_SERVER_URL) + sizeof(PURIN_DEVICE_TOKEN) +
                  sizeof(PURIN_DEVICE_ID) + TERMINAL_SERVER_SIZE + TERMINAL_TOKEN_SIZE + 31];
    int length = snprintf(identity, sizeof(identity), "%s%c%s%c%s", terminal_server_url(), 0,
                          purin_device_id(), 0, terminal_device_token());
    uint8_t binding[32];
    if (length < 0 || (size_t)length >= sizeof(identity) ||
        mbedtls_sha256((const uint8_t *)identity, (size_t)length + 1, binding, 0) != 0) return false;
    size_t size = sizeof(receipt);
    esp_err_t rc = nvs_get_blob(journal, "app_receipt", &receipt, &size);
    if (rc == ESP_OK && (size <= offsetof(status_receipt_t, output) ||
                         receipt.output[size - offsetof(status_receipt_t, output) - 1])) return false;
    if (rc != ESP_OK && rc != ESP_ERR_NVS_NOT_FOUND) return false;
    bool pending = rc == ESP_OK;
    if (pending && memcmp(receipt.binding, binding, sizeof(binding))) {
        if (nvs_erase_key(journal, "app_receipt") != ESP_OK || nvs_commit(journal) != ESP_OK) return false;
        pending = false;
    }
    unsigned mask = device_apps_mask();
    if (!pending && !mask && !PURIN_DEVICE_STATUS_ENABLED && withdrew &&
        registered == (1u << APP_COUNT) - 1 && catalog_enabled == mask) return false;
    if (terminal_network_acquire() != ESP_OK) return false;
    bool touched = false;
    if (!pending) {
        sync_catalog(mask);
        cJSON *res = claim(device_apps_mask());
        touched = res != NULL;
        const cJSON *call = cJSON_GetObjectItemCaseSensitive(res, "call");
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "id"));
        const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "claim_token"));
        if (url_segment(id, sizeof(receipt.id)) && url_segment(token, sizeof(receipt.claim_token))) {
            memcpy(receipt.binding, binding, sizeof(binding));
            strlcpy(receipt.id, id, sizeof(receipt.id));
            strlcpy(receipt.claim_token, token, sizeof(receipt.claim_token));
            receipt.success = false;
            strlcpy(receipt.output, "Device interrupted; outcome unknown", sizeof(receipt.output));
            // Persist before any physical action. Recovery delivers the saved receipt only.
            if (save_receipt(journal)) {
                execute(&receipt, call);
                pending = save_receipt(journal);
            }
        }
        cJSON_Delete(res);
    }
    if (pending && deliver(&receipt)) {
        touched = true;
        if (nvs_erase_key(journal, "app_receipt") == ESP_OK) nvs_commit(journal);
    }
    terminal_network_release();
    return touched;
}
