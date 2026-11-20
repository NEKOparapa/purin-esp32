#include "audio_upload.h"
#include "audio_segment.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "terminal_network.h"
#include "terminal_http.h"

static const char *TAG = "audio_upload";
enum { MAX_SEGMENT_BYTES = 4 * 1024 * 1024 };
static struct {
    QueueHandle_t queue;
    TaskHandle_t worker;
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

esp_err_t audio_upload_prepare(void)
{
    if (!ctx.worker || !terminal_configured()) {
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
    esp_err_t error = terminal_set_auth_headers(client);
    char sequence_text[16], start_text[24], samples_text[24], minutes_text[16], elapsed_text[24];
    snprintf(sequence_text, sizeof(sequence_text), "%" PRIu32, sequence);
    snprintf(start_text, sizeof(start_text), "%" PRIu64, start);
    snprintf(samples_text, sizeof(samples_text), "%" PRIu64, samples);
    snprintf(minutes_text, sizeof(minutes_text), "%" PRIu32, snapshot->window_minutes);
    const int64_t since_snapshot = esp_timer_get_time() - snapshot->captured_at_us;
    const uint64_t elapsed_samples = snapshot->elapsed_samples +
        (since_snapshot > 0 ? (uint64_t)since_snapshot * 16000 / 1000000 : 0);
    snprintf(elapsed_text, sizeof(elapsed_text), "%" PRIu64, elapsed_samples);
    const char *keys[] = {"Content-Type", "X-Recording-ID",
                          "X-Segment-Sequence", "X-Start-Sample", "X-Sample-Count", "X-Sample-Rate", "X-Window-Minutes",
                          "X-Recording-Elapsed-Samples"};
    const char *values[] = {"audio/ogg", snapshot->recording_id,
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
        esp_err_t error = terminal_network_acquire();
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "Network connected; free internal=%u, largest internal block=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            publish(AUDIO_UPLOAD_SENDING, 0);
            error = upload_snapshot(&snapshot);
            terminal_network_release();
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
    case AUDIO_UPLOAD_CONNECTING: return "Connecting network";
    case AUDIO_UPLOAD_SENDING: return "Uploading audio";
    case AUDIO_UPLOAD_ACCEPTED: return "Audio accepted for processing";
    case AUDIO_UPLOAD_FAILED: return "Upload failed; tap BOOT to retry";
    case AUDIO_UPLOAD_NOT_CONFIGURED:
        return terminal_network_mode() == TERMINAL_NETWORK_OFF
            ? "Enable network in Settings" : "Configure network and upload server";
    default: return "";
    }
}
