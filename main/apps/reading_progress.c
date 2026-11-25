#include "reading_progress.h"
#include "terminal_http.h"
#include "terminal_network.h"
#include "terminal_pairing.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char pending_namespace[] = "pr_read_pending";
static const char sent_namespace[] = "pr_read_sent";
enum { JOURNAL_KEY_SIZE = 16 };

typedef struct {
    uint64_t scope;
    int64_t occurred_at;
    uint32_t chapter, offset, total, page, pages;
    bool local, at_end;
    char book_id[16];
    char title[LIBRARY_FILENAME_SIZE];
    char event_id[41];
} reading_update_t;

static SemaphoreHandle_t journal_lock;
static TaskHandle_t uploader;

static bool upload_enabled(void)
{
    nvs_handle_t prefs;
    if (nvs_open(COLLECTION_PREFS_NAMESPACE, NVS_READONLY, &prefs) != ESP_OK) return false;
    uint8_t value = 0;
    bool enabled = nvs_get_u8(prefs, READING_PROGRESS_PREF, &value) == ESP_OK && value == 1;
    nvs_close(prefs);
    return enabled;
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    for (const unsigned char *byte = (const unsigned char *)text; *byte; ++byte) {
        hash = (hash ^ *byte) * UINT64_C(1099511628211);
    }
    return hash * UINT64_C(1099511628211);
}

static uint64_t upload_scope(void)
{
    return hash_text(hash_text(UINT64_C(14695981039346656037), terminal_server_url()),
                     purin_device_id());
}

static uint64_t position(const reading_update_t *update)
{
    return ((uint64_t)update->chapter << 32) | update->offset;
}

static bool read_update(nvs_handle_t pending, const char *key, reading_update_t *out)
{
    size_t size = sizeof(*out);
    return nvs_get_blob(pending, key, out, &size) == ESP_OK && size == sizeof(*out) &&
           out->offset && out->book_id[0] && !out->book_id[sizeof(out->book_id) - 1] &&
           !out->title[sizeof(out->title) - 1] && out->event_id[0] &&
           !out->event_id[sizeof(out->event_id) - 1] && (!out->local || out->total);
}

static bool stage_update(reading_update_t *update)
{
    uint64_t hash = hash_text(hash_text(update->scope, update->local ? "txt" : "agent"),
                              update->book_id);
    char key[JOURNAL_KEY_SIZE];
    snprintf(key, sizeof(key), "%015" PRIx64, hash & UINT64_C(0xfffffffffffffff));
    xSemaphoreTake(journal_lock, portMAX_DELAY);
    nvs_handle_t pending;
    esp_err_t error = nvs_open(pending_namespace, NVS_READWRITE, &pending);
    if (error == ESP_OK) {
        reading_update_t previous;
        bool queued = read_update(pending, key, &previous) && previous.scope == update->scope;
        uint64_t saved = queued ? position(&previous) : 0;
        nvs_handle_t sent;
        if (!queued && nvs_open(sent_namespace, NVS_READONLY, &sent) == ESP_OK) {
            nvs_get_u64(sent, key, &saved);
            nvs_close(sent);
        }
        if (saved != position(update)) {
            uint32_t words[4];
            esp_fill_random(words, sizeof(words));
            snprintf(update->event_id, sizeof(update->event_id),
                     "reading-%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
                     words[0], words[1], words[2], words[3]);
            error = nvs_set_blob(pending, key, update, sizeof(*update));
            if (error == ESP_OK) error = nvs_commit(pending);
        }
        nvs_close(pending);
    }
    xSemaphoreGive(journal_lock);
    return error == ESP_OK;
}

static bool next_update(const char *last_attempt, char key[JOURNAL_KEY_SIZE], reading_update_t *out)
{
    xSemaphoreTake(journal_lock, portMAX_DELAY);
    nvs_handle_t pending;
    key[0] = '\0';
    if (nvs_open(pending_namespace, NVS_READONLY, &pending) == ESP_OK) {
        const uint64_t scope = upload_scope();
        bool chosen_after_last = false;
        nvs_iterator_t iter = NULL;
        for (esp_err_t error = nvs_entry_find("nvs", pending_namespace, NVS_TYPE_BLOB, &iter);
             error == ESP_OK; error = nvs_entry_next(&iter)) {
            nvs_entry_info_t info;
            if (nvs_entry_info(iter, &info) != ESP_OK) continue;
            bool after_last = strcmp(info.key, last_attempt) > 0;
            // Rotate across books so one failed request does not stall the rest.
            if (key[0] && (after_last == chosen_after_last ? strcmp(info.key, key) >= 0 : !after_last)) {
                continue;
            }
            reading_update_t candidate;
            if (!read_update(pending, info.key, &candidate) || candidate.scope != scope) continue;
            *out = candidate;
            strlcpy(key, info.key, JOURNAL_KEY_SIZE);
            chosen_after_last = after_last;
        }
        nvs_release_iterator(iter);
        nvs_close(pending);
    }
    xSemaphoreGive(journal_lock);
    return key[0] != '\0';
}

static bool acknowledge(const char *key, const reading_update_t *update)
{
    xSemaphoreTake(journal_lock, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(sent_namespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_u64(handle, key, position(update));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error == ESP_OK && (error = nvs_open(pending_namespace, NVS_READWRITE, &handle)) == ESP_OK) {
        reading_update_t latest;
        if (read_update(handle, key, &latest) && !strcmp(update->event_id, latest.event_id)) {
            error = nvs_erase_key(handle, key);
            if (error == ESP_OK) error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    xSemaphoreGive(journal_lock);
    return error == ESP_OK;
}

static bool upload_update(const reading_update_t *update)
{
    char text[LIBRARY_FILENAME_SIZE + 128];
    if (update->local) {
        unsigned percent = (uint64_t)update->offset * 100 / update->total;
        snprintf(text, sizeof(text), "阅读进度更新：《%s》读到 %u%%。", update->title, percent);
    } else {
        snprintf(text, sizeof(text), "阅读进度更新：《%s》第 %" PRIu32 " 章，第 %" PRIu32 "/%" PRIu32 " 页。",
                 update->title, update->chapter, update->page, update->pages);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *metadata = body ? cJSON_AddObjectToObject(body, "metadata") : NULL;
    bool ok = metadata && cJSON_AddStringToObject(body, "event_id", update->event_id) &&
              cJSON_AddStringToObject(body, "text", text) &&
              cJSON_AddStringToObject(metadata, "kind", "reading_progress") &&
              cJSON_AddStringToObject(metadata, "book_id", update->book_id) &&
              cJSON_AddStringToObject(metadata, "title", update->title) &&
              cJSON_AddStringToObject(metadata, "format", update->local ? "txt" : "agent") &&
              cJSON_AddNumberToObject(metadata, "offset_bytes", update->offset) &&
              cJSON_AddBoolToObject(metadata, "at_end", update->at_end);
    if (ok) {
        ok = update->local ? cJSON_AddNumberToObject(metadata, "total_bytes", update->total) != NULL :
            cJSON_AddNumberToObject(metadata, "chapter", update->chapter) &&
            cJSON_AddNumberToObject(metadata, "page", update->page) &&
            cJSON_AddNumberToObject(metadata, "pages", update->pages);
    }
    time_t when = update->occurred_at;
    struct tm utc;
    char stamp[24];
    if (ok && gmtime_r(&when, &utc) && utc.tm_year >= 120 && utc.tm_year <= 199 &&
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc)) {
        ok = cJSON_AddStringToObject(body, "occurred_at", stamp) != NULL;
    }
    cJSON *receipt = NULL;
    if (ok && terminal_network_acquire() == ESP_OK) {
        if (upload_enabled()) {
            char status[64];
            receipt = terminal_http_json_bounded(status, sizeof(status), HTTP_METHOD_POST,
                                                 "/v1/slices", body, 202, 1024, NULL);
        }
        terminal_network_release();
    }
    const char *event_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "event_id"));
    ok = event_id && *event_id;
    cJSON_Delete(receipt);
    cJSON_Delete(body);
    return ok;
}

static void upload_worker(void *unused)
{
    (void)unused;
    char last_attempt[JOURNAL_KEY_SIZE] = "";
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        char key[JOURNAL_KEY_SIZE];
        reading_update_t update;
        if (upload_enabled() && next_update(last_attempt, key, &update)) {
            strlcpy(last_attempt, key, sizeof(last_attempt));
            bool sent = terminal_configured() && upload_update(&update) && acknowledge(key, &update);
            wait = pdMS_TO_TICKS(sent ? 1000 : 30000);
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

void reading_progress_wake(void)
{
    if (!upload_enabled()) return;
    if (!journal_lock) journal_lock = xSemaphoreCreateMutex();
    if (!journal_lock) return;
    if (!uploader && xTaskCreate(upload_worker, "reading_upload", 10240, NULL, 1, &uploader) != pdPASS) {
        return;
    }
    xTaskNotifyGive(uploader);
}

bool reading_progress_submit(const library_book_t *book, const library_section_t *section,
                             uint32_t page_end, uint32_t page, uint32_t pages)
{
    if (!upload_enabled()) return true;
    if (!journal_lock) journal_lock = xSemaphoreCreateMutex();
    if (!journal_lock || !page_end || page_end > section->length) return false;
    reading_update_t update = {
        .scope = upload_scope(), .occurred_at = time(NULL), .local = book->local,
        .chapter = book->local ? 0 : section->number, .offset = section->start + page_end,
        .total = section->total, .page = page, .pages = pages,
        .at_end = !section->next && page == pages,
    };
    strlcpy(update.book_id, book->local ? section->bookmark_key : book->name, sizeof(update.book_id));
    strlcpy(update.title, section->title, sizeof(update.title));
    bool staged = update.book_id[0] && stage_update(&update);
    if (staged) reading_progress_wake();
    return staged;
}
