#include "audio_upload.h"
#include "audio_segment.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#ifdef WINTERMUTE_LOCAL_UPLOAD_CONFIG
#include "upload_config.local.h"
#else
#include "upload_config.example.h"
#endif

static const char *TAG = "audio_upload";
enum { WIFI_CONNECTED = BIT0, WIFI_ENABLED = BIT1, MAX_SEGMENT_BYTES = 4 * 1024 * 1024 };
static struct {
    QueueHandle_t queue;
    TaskHandle_t worker;
    EventGroupHandle_t wifi_events;
    bool wifi_ready;
    audio_upload_status_t status;
} ctx;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void publish(audio_upload_state_t state, uint32_t sent)
{
    portENTER_CRITICAL(&lock);
    ctx.status = (audio_upload_status_t){.state = state, .sent_segments = sent};
    portEXIT_CRITICAL(&lock);
}

audio_upload_status_t audio_upload_get_status(void)
{
    portENTER_CRITICAL(&lock);
    audio_upload_status_t status = ctx.status;
    portEXIT_CRITICAL(&lock);
    return status;
}

bool audio_upload_busy(void)
{
    audio_upload_state_t state = audio_upload_get_status().state;
    return state == AUDIO_UPLOAD_PREPARING || state == AUDIO_UPLOAD_CONNECTING || state == AUDIO_UPLOAD_SENDING;
}

static bool configured(void)
{
    return WINTERMUTE_WIFI_SSID[0] && WINTERMUTE_SERVER_URL[0] && WINTERMUTE_TERMINAL_ID[0] &&
           WINTERMUTE_TERMINAL_TOKEN[0] &&
           (!strncmp(WINTERMUTE_SERVER_URL, "https://", 8) || !strncmp(WINTERMUTE_SERVER_URL, "http://", 7));
}

esp_err_t audio_upload_prepare(void)
{
    if (!ctx.worker || !configured()) {
        publish(AUDIO_UPLOAD_NOT_CONFIGURED, 0);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&lock);
    if (ctx.status.state == AUDIO_UPLOAD_PREPARING || ctx.status.state == AUDIO_UPLOAD_CONNECTING ||
        ctx.status.state == AUDIO_UPLOAD_SENDING) {
        portEXIT_CRITICAL(&lock);
        return ESP_ERR_INVALID_STATE;
    }
    ctx.status = (audio_upload_status_t){.state = AUDIO_UPLOAD_PREPARING};
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}

void audio_upload_fail(void)
{
    publish(AUDIO_UPLOAD_FAILED, audio_upload_get_status().sent_segments);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(ctx.wifi_events, WIFI_CONNECTED);
    } else if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(ctx.wifi_events, WIFI_CONNECTED);
        if (xEventGroupGetBits(ctx.wifi_events) & WIFI_ENABLED) {
            (void)esp_wifi_connect();
        }
    }
}

static esp_err_t wifi_init(void)
{
    if (ctx.wifi_ready) {
        return ESP_OK;
    }
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        return error; // 不擦除已有 NVS。
    }
    error = esp_netif_init();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    ctx.wifi_events = xEventGroupCreate();
    if (!ctx.wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if (error != ESP_OK) {
        esp_netif_destroy_default_wifi(netif);
        vEventGroupDelete(ctx.wifi_events);
        ctx.wifi_events = NULL;
        return error;
    }
    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (error == ESP_OK) {
        error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    }
    wifi_config_t config = {0};
    if (strlen(WINTERMUTE_WIFI_SSID) > sizeof(config.sta.ssid) ||
        strlen(WINTERMUTE_WIFI_PASSWORD) > sizeof(config.sta.password)) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) {
        memcpy(config.sta.ssid, WINTERMUTE_WIFI_SSID, strlen(WINTERMUTE_WIFI_SSID));
        memcpy(config.sta.password, WINTERMUTE_WIFI_PASSWORD, strlen(WINTERMUTE_WIFI_PASSWORD));
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (error != ESP_OK) {
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event);
        (void)esp_wifi_deinit();
        esp_netif_destroy_default_wifi(netif);
        vEventGroupDelete(ctx.wifi_events);
        ctx.wifi_events = NULL;
        return error;
    }
    ctx.wifi_ready = true;
    return ESP_OK;
}

static esp_err_t connect_wifi(void)
{
    esp_err_t error = wifi_init();
    if (error != ESP_OK) {
        return error;
    }
    xEventGroupClearBits(ctx.wifi_events, WIFI_CONNECTED);
    xEventGroupSetBits(ctx.wifi_events, WIFI_ENABLED);
    error = esp_wifi_start();
    if (error != ESP_OK) {
        return error;
    }
    EventBits_t bits = xEventGroupWaitBits(ctx.wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    return bits & WIFI_CONNECTED ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t send_segment(const audio_upload_snapshot_t *snapshot, uint32_t sequence,
                               uint64_t start, uint64_t samples, uint64_t bytes)
{
    char path[256];
    int length = snprintf(path, sizeof(path), "%s/recordings/%06" PRIu32 "/%06" PRIu32 ".ogg",
                          snapshot->root, snapshot->session_id, sequence);
    if (length < 0 || (size_t)length >= sizeof(path) || !bytes || bytes > MAX_SEGMENT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 0 || (uint64_t)info.st_size != bytes) {
        return ESP_FAIL;
    }
    FILE *audio = fopen(path, "rb");
    if (!audio) {
        return ESP_FAIL;
    }
    char url[512];
    length = snprintf(url, sizeof(url), "%s/v1/audio/slices", WINTERMUTE_SERVER_URL);
    if (length < 0 || (size_t)length >= sizeof(url)) {
        fclose(audio);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(audio);
        return ESP_ERR_NO_MEM;
    }
    char authorization[512];
    length = snprintf(authorization, sizeof(authorization), "Bearer %s", WINTERMUTE_TERMINAL_TOKEN);
    esp_err_t error = length < 0 || (size_t)length >= sizeof(authorization) ? ESP_ERR_INVALID_SIZE : ESP_OK;
    char sequence_text[16], start_text[24], samples_text[24], minutes_text[16], elapsed_text[24];
    snprintf(sequence_text, sizeof(sequence_text), "%" PRIu32, sequence);
    snprintf(start_text, sizeof(start_text), "%" PRIu64, start);
    snprintf(samples_text, sizeof(samples_text), "%" PRIu64, samples);
    snprintf(minutes_text, sizeof(minutes_text), "%" PRIu32, snapshot->window_minutes);
    const int64_t since_snapshot = esp_timer_get_time() - snapshot->captured_at_us;
    const uint64_t elapsed_samples = snapshot->elapsed_samples +
        (since_snapshot > 0 ? (uint64_t)since_snapshot * 16000 / 1000000 : 0);
    snprintf(elapsed_text, sizeof(elapsed_text), "%" PRIu64, elapsed_samples);
    const char *keys[] = {"Content-Type", "X-Terminal-ID", "Authorization", "X-Recording-ID",
                          "X-Segment-Sequence", "X-Start-Sample", "X-Sample-Count", "X-Sample-Rate", "X-Window-Minutes",
                          "X-Recording-Elapsed-Samples"};
    const char *values[] = {"audio/ogg", WINTERMUTE_TERMINAL_ID, authorization, snapshot->recording_id,
                            sequence_text, start_text, samples_text, "16000", minutes_text, elapsed_text};
    for (unsigned i = 0; error == ESP_OK && i < sizeof(keys) / sizeof(keys[0]); ++i) {
        error = esp_http_client_set_header(client, keys[i], values[i]);
    }
    if (error == ESP_OK) {
        error = esp_http_client_open(client, (int)bytes);
    }
    char buffer[2048];
    uint64_t remaining = bytes;
    while (error == ESP_OK && remaining) {
        size_t amount = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (fread(buffer, 1, amount, audio) != amount) {
            error = ESP_FAIL;
            break;
        }
        size_t offset = 0;
        while (offset < amount) {
            int written = esp_http_client_write(client, buffer + offset, (int)(amount - offset));
            if (written <= 0) {
                error = ESP_FAIL;
                break;
            }
            offset += (size_t)written;
        }
        remaining -= amount;
        // 上传低于采集/编码优先级；每个 SD 读取块后让出 CPU。
        vTaskDelay(1);
    }
    if (error == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0 || esp_http_client_get_status_code(client) != 202) {
            ESP_LOGW(TAG, "Segment rejected, HTTP %d", esp_http_client_get_status_code(client));
            error = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(audio);
    return error;
}

static esp_err_t upload_snapshot(const audio_upload_snapshot_t *snapshot)
{
    char path[256];
    int length = snprintf(path, sizeof(path), "%s/recordings/%06" PRIu32 "/index.jsonl", snapshot->root, snapshot->session_id);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *index = fopen(path, "rb");
    if (!index) {
        return ESP_FAIL;
    }
    char line[320];
    uint64_t expected_start = 0;
    uint32_t expected_sequence = 1;
    esp_err_t error = ESP_FAIL;
    // 首行由 recording_store 写入；每次重试始终从起点开始，服务端幂等去重。
    if (!fgets(line, sizeof(line), index) || !strchr(line, '\n')) {
        fclose(index);
        return ESP_FAIL;
    }
    while (expected_start < snapshot->samples && fgets(line, sizeof(line), index)) {
        audio_segment_t segment;
        if (!audio_segment_parse(line, expected_sequence, expected_start, snapshot->samples, &segment)) {
            break;
        }
        error = send_segment(snapshot, segment.sequence, segment.start, segment.samples, segment.bytes);
        if (error != ESP_OK) {
            break;
        }
        expected_start += segment.samples;
        publish(AUDIO_UPLOAD_SENDING, expected_sequence);
        ++expected_sequence;
    }
    fclose(index);
    return expected_start == snapshot->samples ? ESP_OK : (error == ESP_OK ? ESP_FAIL : error);
}

static void upload_worker(void *arg)
{
    (void)arg;
    audio_upload_snapshot_t snapshot;
    for (;;) {
        xQueueReceive(ctx.queue, &snapshot, portMAX_DELAY);
        publish(AUDIO_UPLOAD_CONNECTING, 0);
        esp_err_t error = connect_wifi();
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi connected; free internal=%u, largest internal block=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            publish(AUDIO_UPLOAD_SENDING, 0);
            error = upload_snapshot(&snapshot);
        }
        if (ctx.wifi_ready) {
            xEventGroupClearBits(ctx.wifi_events, WIFI_ENABLED | WIFI_CONNECTED);
            (void)esp_wifi_stop();
        }
        ESP_LOGI(TAG, "Upload %s: %s", error == ESP_OK ? "accepted" : "failed", esp_err_to_name(error));
        publish(error == ESP_OK ? AUDIO_UPLOAD_ACCEPTED : AUDIO_UPLOAD_FAILED, audio_upload_get_status().sent_segments);
    }
}

esp_err_t audio_upload_init(void)
{
    if (ctx.worker) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx.queue = xQueueCreate(1, sizeof(audio_upload_snapshot_t));
    if (!ctx.queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(upload_worker, "audio_upload", 12288, NULL, 1, &ctx.worker) != pdPASS) {
        vQueueDelete(ctx.queue);
        ctx.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_upload_submit(const audio_upload_snapshot_t *snapshot)
{
    if (!snapshot || !snapshot->samples || !ctx.queue || audio_upload_get_status().state != AUDIO_UPLOAD_PREPARING) {
        audio_upload_fail();
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(ctx.queue, snapshot, 0) != pdTRUE) {
        audio_upload_fail();
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char *audio_upload_status_text(audio_upload_state_t state)
{
    switch (state) {
    case AUDIO_UPLOAD_PREPARING: return "Preparing audio";
    case AUDIO_UPLOAD_CONNECTING: return "Connecting Wi-Fi";
    case AUDIO_UPLOAD_SENDING: return "Uploading audio";
    case AUDIO_UPLOAD_ACCEPTED: return "Audio accepted for processing";
    case AUDIO_UPLOAD_FAILED: return "Upload failed; tap BOOT to retry";
    case AUDIO_UPLOAD_NOT_CONFIGURED: return "Configure Wi-Fi and upload server";
    default: return "";
    }
}
