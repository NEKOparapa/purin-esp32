#include "terminal_service.h"
#include "terminal_http.h"
#include "terminal_network.h"
#include "terminal_pairing.h"
#include "terminal_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

typedef struct {
    char value[32];
} recorder_update_t;

// Keep the layout of existing, idempotent recorder-state events in flash.
typedef struct {
    char id[33];
    char state[32];
} capture_t;

static QueueHandle_t recorder_updates;
static nvs_handle_t journal;

static bool send_recorder_state(const char *state)
{
    capture_t event = {0};
    size_t size = sizeof(event);
    esp_err_t err = nvs_get_blob(journal, "capture", &event, &size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH)
        return false;
    if (err != ESP_OK || size != sizeof(event) ||
        event.id[sizeof(event.id)-1] || strspn(event.id, "0123456789abcdef") != 32 ||
        strncmp(state, event.state, sizeof(event.state))) {
        uint32_t rnd[4];
        esp_fill_random(rnd, sizeof(rnd));
        snprintf(event.id, sizeof(event.id), "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
                 rnd[0], rnd[1], rnd[2], rnd[3]);
        strlcpy(event.state, state, sizeof(event.state));
        if (nvs_set_blob(journal, "capture", &event, sizeof(event)) != ESP_OK ||
            nvs_commit(journal) != ESP_OK) return false;
    }
    if (terminal_network_acquire() != ESP_OK) return false;
    char status[64];
    cJSON *body = cJSON_CreateObject();
    cJSON *receipt = NULL;
    if (body && cJSON_AddStringToObject(body, "event_id", event.id) &&
        cJSON_AddStringToObject(body, "state_key", "recorder.state") &&
        cJSON_AddStringToObject(body, "value", event.state)) {
        receipt = terminal_http_json_bounded(status, sizeof(status), HTTP_METHOD_POST,
                                            "/v1/perceptions", body, 202, 4096, NULL);
    }
    terminal_network_release();
    cJSON_Delete(body);
    bool confirmed = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(receipt, "event_id"));
    cJSON_Delete(receipt);
    return confirmed && nvs_erase_key(journal, "capture") == ESP_OK && nvs_commit(journal) == ESP_OK;
}

static void announce_presence(void)
{
    if (terminal_network_link_state() == TERMINAL_LINK_FAILED || terminal_network_acquire() != ESP_OK)
        return;
    char status[48];
    cJSON_Delete(terminal_http_json_bounded(status, sizeof(status), HTTP_METHOD_GET,
                                            "/v1/devices/online", NULL, 200, 16 * 1024, NULL));
    terminal_network_release();
}

static TickType_t until_due(TickType_t last, TickType_t period, TickType_t now)
{
    const TickType_t elapsed = now - last;
    return elapsed < period ? period - elapsed : 0;
}

static void service_worker(void *arg)
{
    (void)arg;
    const TickType_t tool_period = pdMS_TO_TICKS(6000);
    const TickType_t presence_period = pdMS_TO_TICKS(10000);
    const TickType_t retry_period = pdMS_TO_TICKS(1500);
    recorder_update_t pending = {0};
    TickType_t now = xTaskGetTickCount();
    TickType_t last_tools = now - tool_period;
    TickType_t last_presence = now - presence_period;
    TickType_t last_report = now - retry_period;
    for (;;) {
        now = xTaskGetTickCount();
        TickType_t wait = terminal_device_token()[0] ? pdMS_TO_TICKS(5000) : portMAX_DELAY;
        if (terminal_configured()) {
            wait = until_due(last_presence, presence_period, now);
            if (terminal_tools_enabled()) {
                TickType_t due = until_due(last_tools, tool_period, now);
                if (due < wait) wait = due;
            }
            if (pending.value[0]) {
                TickType_t due = until_due(last_report, retry_period, now);
                if (due < wait) wait = due;
            }
        }
        if (xQueueReceive(recorder_updates, &pending, wait) == pdTRUE)
            last_report = xTaskGetTickCount() - retry_period;
        if (!terminal_configured()) continue;
        if (pending.value[0] && !until_due(last_report, retry_period, xTaskGetTickCount())) {
            if (send_recorder_state(pending.value)) pending.value[0] = '\0';
            last_report = xTaskGetTickCount();
        }
        if (terminal_tools_enabled() && !until_due(last_tools, tool_period, xTaskGetTickCount())) {
            bool touched = terminal_tools_poll(journal);
            last_tools = xTaskGetTickCount();
            if (touched) last_presence = last_tools;
        }
        if (!until_due(last_presence, presence_period, xTaskGetTickCount())) {
            announce_presence();
            last_presence = xTaskGetTickCount();
        }
    }
}

esp_err_t terminal_service_init(void)
{
    if (recorder_updates) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    // Preserve the fixed-credential journal as well as the per-pairing journals.
    char journal_name[16] = "pr_chat";
    if (terminal_is_paired())
        snprintf(journal_name, sizeof(journal_name), "pr_%.12s", purin_device_id() + 6);
    err = nvs_open(journal_name, NVS_READWRITE, &journal);
    if (err != ESP_OK) return err;
    recorder_updates = xQueueCreate(1, sizeof(recorder_update_t));
    if (recorder_updates && xTaskCreate(service_worker, "terminal", 10240, NULL, 1, NULL) == pdPASS)
        return ESP_OK;
    if (recorder_updates) vQueueDelete(recorder_updates);
    recorder_updates = NULL;
    nvs_close(journal);
    return ESP_ERR_NO_MEM;
}

void terminal_service_report_recorder_state(const char *state)
{
    if (!recorder_updates || !state || !*state) return;
    recorder_update_t update;
    strlcpy(update.value, state, sizeof(update.value));
    xQueueOverwrite(recorder_updates, &update);
}
