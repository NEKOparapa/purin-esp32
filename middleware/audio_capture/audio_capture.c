#include "audio_capture.h"

#include <stdatomic.h>
#include "audio_pcm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "audio_capture";

struct audio_capture {
    TaskHandle_t task;
    QueueHandle_t queue;
    StaticQueue_t queue_control;
    uint8_t *queue_buffer;
    i2s_microphone_handle_t microphone;
    unsigned pcm_shift;
    atomic_bool stop_requested;
    atomic_bool done;
    atomic_int error;
};

static void capture_task(void *arg)
{
    audio_capture_handle_t capture = arg;
    int32_t stereo[AUDIO_CAPTURE_FRAME_SAMPLES * 2];
    audio_capture_block_t block;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        audio_capture_error_t failure = AUDIO_CAPTURE_ERROR_NONE;
        bool started = false;
        if (!atomic_load(&capture->stop_requested)) {
            esp_err_t err = i2s_microphone_start(capture->microphone);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Microphone start: %s", esp_err_to_name(err));
                failure = AUDIO_CAPTURE_ERROR_MICROPHONE;
            } else {
                started = true;
            }
        }
        int64_t last_audio = esp_timer_get_time();
        while (started && failure == AUDIO_CAPTURE_ERROR_NONE && !atomic_load(&capture->stop_requested)) {
            size_t frames = 0;
            esp_err_t err = i2s_microphone_read(capture->microphone, stereo,
                                               AUDIO_CAPTURE_FRAME_SAMPLES, &frames, 100);
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Microphone read: %s", esp_err_to_name(err));
                failure = err == ESP_ERR_INVALID_SIZE ? AUDIO_CAPTURE_ERROR_OVERRUN : AUDIO_CAPTURE_ERROR_MICROPHONE;
                break;
            }
            if (frames > AUDIO_CAPTURE_FRAME_SAMPLES) {
                failure = AUDIO_CAPTURE_ERROR_MICROPHONE;
                break;
            }
            if (frames > 0) {
                last_audio = esp_timer_get_time();
                block.count = frames;
                for (size_t i = 0; i < frames; ++i) {
                    block.pcm[i] = audio_pcm_mix_sample(stereo[2 * i], stereo[2 * i + 1], capture->pcm_shift);
                }
                // 不通过阻塞采集掩盖丢帧；缓冲耗尽即终止该次输入流。
                if (xQueueSend(capture->queue, &block, 0) != pdTRUE) {
                    failure = AUDIO_CAPTURE_ERROR_OVERRUN;
                }
            } else if (esp_timer_get_time() - last_audio >= 1000000) {
                failure = AUDIO_CAPTURE_ERROR_NO_AUDIO;
            }
        }
        if (started) {
            esp_err_t err = i2s_microphone_stop(capture->microphone);
            if (err != ESP_OK && failure == AUDIO_CAPTURE_ERROR_NONE) {
                failure = AUDIO_CAPTURE_ERROR_MICROPHONE;
            }
        }
        atomic_store(&capture->error, failure);
        // 发布 done 前，所有入队操作和驱动 stop 均已完成。
        atomic_store(&capture->done, true);
    }
}

esp_err_t audio_capture_create(audio_capture_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    audio_capture_handle_t capture = heap_caps_calloc(1, sizeof(*capture),
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!capture) {
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&capture->stop_requested, true);
    atomic_init(&capture->done, true);
    atomic_init(&capture->error, AUDIO_CAPTURE_ERROR_NONE);
    const size_t bytes = AUDIO_CAPTURE_QUEUE_BLOCKS * sizeof(audio_capture_block_t);
    capture->queue_buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!capture->queue_buffer) {
        goto fail;
    }
    capture->queue = xQueueCreateStatic(AUDIO_CAPTURE_QUEUE_BLOCKS, sizeof(audio_capture_block_t),
                                        capture->queue_buffer, &capture->queue_control);
    if (!capture->queue) {
        goto fail;
    }
    // 默认 xTaskCreate 的 DRAM 栈；任务独占麦克风的启停与有限等待读取。
    if (xTaskCreate(capture_task, "mic_capture", 8192, capture, 4, &capture->task) != pdPASS) {
        goto fail;
    }
    *out = capture;
    return ESP_OK;

fail:
    if (capture->queue) {
        vQueueDelete(capture->queue);
    }
    heap_caps_free(capture->queue_buffer);
    heap_caps_free(capture);
    return ESP_ERR_NO_MEM;
}

esp_err_t audio_capture_start(audio_capture_handle_t capture, i2s_microphone_handle_t microphone,
                               unsigned pcm_shift)
{
    if (!capture || !microphone || pcm_shift > 31) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&capture->done)) {
        return ESP_ERR_INVALID_STATE;
    }
    xQueueReset(capture->queue);
    capture->microphone = microphone;
    capture->pcm_shift = pcm_shift;
    atomic_store(&capture->error, AUDIO_CAPTURE_ERROR_NONE);
    atomic_store(&capture->stop_requested, false);
    atomic_store(&capture->done, false);
    xTaskNotifyGive(capture->task);
    return ESP_OK;
}

void audio_capture_request_stop(audio_capture_handle_t capture)
{
    if (capture) {
        atomic_store(&capture->stop_requested, true);
    }
}

esp_err_t audio_capture_read(audio_capture_handle_t capture, audio_capture_block_t *out,
                              uint32_t timeout_ms)
{
    if (!capture || !out || timeout_ms > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    out->count = 0;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !ticks) {
        ticks = 1;
    }
    return xQueueReceive(capture->queue, out, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

audio_capture_status_t audio_capture_get_status(audio_capture_handle_t capture)
{
    if (!capture) {
        return (audio_capture_status_t) {.done = true, .error = AUDIO_CAPTURE_ERROR_MICROPHONE};
    }
    const bool done = atomic_load(&capture->done);
    return (audio_capture_status_t) {
        .done = done,
        .error = (audio_capture_error_t)atomic_load(&capture->error),
        .queued_blocks = uxQueueMessagesWaiting(capture->queue),
    };
}

size_t audio_capture_stack_free(audio_capture_handle_t capture)
{
    return capture ? (size_t)uxTaskGetStackHighWaterMark(capture->task) : 0;
}

esp_err_t audio_capture_destroy(audio_capture_handle_t capture)
{
    if (!capture) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&capture->done)) {
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelete(capture->task);
    vQueueDelete(capture->queue);
    heap_caps_free(capture->queue_buffer);
    heap_caps_free(capture);
    return ESP_OK;
}
