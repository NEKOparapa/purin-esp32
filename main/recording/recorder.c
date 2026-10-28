#include "recorder.h"

#include <inttypes.h>
#include "board.h"
#include "audio_capture.h"
#include "recording_writer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "recorder";
enum { SEGMENT_SECONDS = 300 }; // 应用策略；存储模块接受明确的样本数。
_Static_assert((int)AUDIO_CAPTURE_SAMPLE_RATE == (int)RECORDING_WRITER_SAMPLE_RATE, "PCM sample rate mismatch");

static struct {
    TaskHandle_t worker;
    audio_capture_handle_t capture;
    recorder_status_t status;
} ctx;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

static void publish_state(recorder_state_t state, recorder_error_t error)
{
    portENTER_CRITICAL(&status_lock);
    ctx.status.state = state;
    ctx.status.error = error;
    portEXIT_CRITICAL(&status_lock);
}

static void publish_progress(recording_writer_stats_t stats, bool received_audio)
{
    portENTER_CRITICAL(&status_lock);
    ctx.status.samples = stats.samples;
    ctx.status.saved_segments = stats.saved_segments;
    if (received_audio && ctx.status.state == RECORDER_STARTING) {
        ctx.status.state = RECORDER_RECORDING;
    }
    portEXIT_CRITICAL(&status_lock);
}

recorder_status_t recorder_get_status(void)
{
    portENTER_CRITICAL(&status_lock);
    recorder_status_t result = ctx.status;
    portEXIT_CRITICAL(&status_lock);
    return result;
}

// 中间件只报告自己的错误；业务文案与状态转换集中在应用。
static recorder_error_t capture_error(audio_capture_error_t error)
{
    switch (error) {
    case AUDIO_CAPTURE_ERROR_NONE: return RECORDER_ERROR_NONE;
    case AUDIO_CAPTURE_ERROR_NO_AUDIO: return RECORDER_ERROR_NO_AUDIO;
    case AUDIO_CAPTURE_ERROR_OVERRUN: return RECORDER_ERROR_OVERRUN;
    case AUDIO_CAPTURE_ERROR_MICROPHONE: return RECORDER_ERROR_MICROPHONE;
    default: return RECORDER_ERROR_MICROPHONE;
    }
}

static recorder_error_t writer_error(recording_write_result_t error)
{
    switch (error) {
    case RECORDING_WRITE_OK: return RECORDER_ERROR_NONE;
    case RECORDING_WRITE_NO_MEMORY: return RECORDER_ERROR_MEMORY;
    case RECORDING_WRITE_CARD_FULL: return RECORDER_ERROR_CARD_FULL;
    case RECORDING_WRITE_STORAGE_ERROR: return RECORDER_ERROR_STORAGE;
    default: return RECORDER_ERROR_ENCODER;
    }
}

static void run_session(void)
{
    recorder_error_t failure = RECORDER_ERROR_NONE;
    bool mounted = false;
    recording_writer_handle_t writer = NULL;
    i2s_microphone_handle_t microphone = NULL;
    int64_t last_sync = esp_timer_get_time();
    int64_t max_write_us = 0;
    uint64_t write_us = 0;
    uint64_t write_samples = 0;
    audio_capture_block_t block;
    board_sdcard_info_t info;

    esp_err_t err = board_sdcard_mount(&info);
    if (err != ESP_OK) {
        failure = RECORDER_ERROR_SD;
        goto cleanup;
    }
    mounted = true;
    err = board_microphone_init(&microphone);
    if (err != ESP_OK) {
        failure = RECORDER_ERROR_MICROPHONE;
        goto cleanup;
    }
    failure = writer_error(recording_writer_open(board_sdcard_path(),
                           (uint64_t)RECORDING_WRITER_SAMPLE_RATE * SEGMENT_SECONDS, &writer));
    if (failure != RECORDER_ERROR_NONE) {
        goto cleanup;
    }
    err = audio_capture_start(ctx.capture, microphone, board_microphone_pcm_shift());
    if (err != ESP_OK) {
        failure = RECORDER_ERROR_MICROPHONE;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Starting session %06" PRIu32 "; free internal=%u PSRAM=%u",
             recording_writer_get_stats(writer).session_id,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (;;) {
        audio_capture_status_t input = audio_capture_get_status(ctx.capture);
        failure = capture_error(input.error);
        if (failure != RECORDER_ERROR_NONE || (input.done && input.queued_blocks == 0)) {
            break;
        }
        err = audio_capture_read(ctx.capture, &block, 50);
        if (err == ESP_OK) {
            int64_t before = esp_timer_get_time();
            failure = writer_error(recording_writer_write(writer, block.pcm, block.count));
            int64_t elapsed = esp_timer_get_time() - before;
            if (elapsed > max_write_us) {
                max_write_us = elapsed;
            }
            write_us += elapsed;
            write_samples += block.count;
            publish_progress(recording_writer_get_stats(writer), failure == RECORDER_ERROR_NONE);
            if (failure != RECORDER_ERROR_NONE) {
                break;
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            failure = RECORDER_ERROR_MICROPHONE;
            break;
        }
        if (esp_timer_get_time() - last_sync >= 1000000) {
            failure = writer_error(recording_writer_sync(writer));
            if (failure != RECORDER_ERROR_NONE) {
                break;
            }
            last_sync = esp_timer_get_time();
        }
    }

cleanup:
    // 仅正常停止会在上面的循环排空队列；故障立即停采，存储层保留未完成文件。
    audio_capture_request_stop(ctx.capture);
    audio_capture_status_t input;
    do {
        input = audio_capture_get_status(ctx.capture);
        if (!input.done) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } while (!input.done);
    if (failure == RECORDER_ERROR_NONE) {
        failure = capture_error(input.error);
    }
    if (writer) {
        recording_writer_stats_t stats;
        if (failure == RECORDER_ERROR_NONE) {
            publish_state(RECORDER_STOPPING, RECORDER_ERROR_NONE);
            failure = writer_error(recording_writer_finish(&writer, &stats));
        } else {
            // 清理错误不能覆盖最初的故障。
            (void)recording_writer_abort(&writer, &stats);
        }
        publish_progress(stats, false);
    }
    if (mounted) {
        err = board_sdcard_unmount();
        if (err != ESP_OK && failure == RECORDER_ERROR_NONE) {
            failure = RECORDER_ERROR_STORAGE;
        }
    }
    ESP_LOGI(TAG, "Stopped: %s; encode/write mean=%" PRIu64 " us/20ms, max=%lld us; stack free=%u/%u",
             recorder_error_text(failure), write_samples ? write_us * AUDIO_CAPTURE_FRAME_SAMPLES / write_samples : 0,
             (long long)max_write_us, (unsigned)uxTaskGetStackHighWaterMark(ctx.worker),
             (unsigned)audio_capture_stack_free(ctx.capture));
    publish_state(failure == RECORDER_ERROR_NONE ? RECORDER_IDLE : RECORDER_ERROR, failure);
}

static void recording_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_session();
    }
}

esp_err_t recorder_init(void)
{
    if (ctx.worker) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = audio_capture_create(&ctx.capture);
    if (err != ESP_OK) {
        publish_state(RECORDER_ERROR, RECORDER_ERROR_MEMORY);
        return err;
    }
    // 编码/写卡仍由唯一工作任务串行执行，内部栈保留现有大小。
    if (xTaskCreate(recording_worker, "recording", 32768, NULL, 3, &ctx.worker) != pdPASS) {
        audio_capture_destroy(ctx.capture);
        ctx.capture = NULL;
        publish_state(RECORDER_ERROR, RECORDER_ERROR_MEMORY);
        return ESP_ERR_NO_MEM;
    }
    publish_state(RECORDER_IDLE, RECORDER_ERROR_NONE);
    return ESP_OK;
}

esp_err_t recorder_start(void)
{
    portENTER_CRITICAL(&status_lock);
    if (!ctx.worker || (ctx.status.state != RECORDER_IDLE && ctx.status.state != RECORDER_ERROR)) {
        portEXIT_CRITICAL(&status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ctx.status = (recorder_status_t) {.state = RECORDER_STARTING};
    portEXIT_CRITICAL(&status_lock);
    xTaskNotifyGive(ctx.worker);
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    portENTER_CRITICAL(&status_lock);
    if (!ctx.worker || ctx.status.state != RECORDER_RECORDING) {
        portEXIT_CRITICAL(&status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ctx.status.state = RECORDER_STOPPING;
    // request_stop 仅作原子标记，保持状态切换和停止请求一起完成。
    audio_capture_request_stop(ctx.capture);
    portEXIT_CRITICAL(&status_lock);
    return ESP_OK;
}

const char *recorder_error_text(recorder_error_t error)
{
    switch (error) {
    case RECORDER_ERROR_NONE: return "Ready";
    case RECORDER_ERROR_MEMORY: return "Not enough memory";
    case RECORDER_ERROR_SD: return "SD card unavailable";
    case RECORDER_ERROR_STORAGE: return "SD write failed";
    case RECORDER_ERROR_CARD_FULL: return "SD card full";
    case RECORDER_ERROR_MICROPHONE: return "Microphone error";
    case RECORDER_ERROR_NO_AUDIO: return "No audio input";
    case RECORDER_ERROR_OVERRUN: return "Audio buffer overrun";
    case RECORDER_ERROR_ENCODER: return "Audio encoding failed";
    default: return "Recording error";
    }
}
