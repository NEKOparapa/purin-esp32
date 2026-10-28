#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "i2s_microphone.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUDIO_CAPTURE_SAMPLE_RATE = 16000,
    AUDIO_CAPTURE_FRAME_SAMPLES = 320,
    AUDIO_CAPTURE_QUEUE_BLOCKS = 100, // 16 kHz 下约 2 秒 PCM，存放于 PSRAM。
};

typedef struct audio_capture *audio_capture_handle_t;
typedef struct {
    size_t count;
    int16_t pcm[AUDIO_CAPTURE_FRAME_SAMPLES];
} audio_capture_block_t;

typedef enum {
    AUDIO_CAPTURE_ERROR_NONE,
    AUDIO_CAPTURE_ERROR_MICROPHONE,
    AUDIO_CAPTURE_ERROR_NO_AUDIO,
    AUDIO_CAPTURE_ERROR_OVERRUN,
} audio_capture_error_t;

typedef struct {
    bool done; // 驱动已停止且不会再入队；true 时队列仍可能有待消费 PCM。
    audio_capture_error_t error;
    size_t queued_blocks;
} audio_capture_status_t;

// 创建采集任务及队列，尚不操作麦克风。无板级、文件、编码器或页面依赖。
esp_err_t audio_capture_create(audio_capture_handle_t *out);

/* 麦克风由调用方准备为 16 kHz stereo32，所有权不转移；采集中不能由外部操作。
 * 只有上次 done 后可开始，清除上次剩余队列及错误。启动为异步，实际驱动失败
 * 通过 get_status 报告。start/read/destroy 由一个所有者串行调用。
 */
esp_err_t audio_capture_start(audio_capture_handle_t handle, i2s_microphone_handle_t microphone,
                               unsigned pcm_shift);

// 可跨任务调用；仅提出请求，驱动 start/read/stop 始终由采集任务串行执行。
// 当前有限超时 read 可返回最后一块；调用方须等 done 并排空队列后正常收尾。
void audio_capture_request_stop(audio_capture_handle_t handle);

// 从队列取单声道 PCM；timeout_ms=0 为立即返回，上限1000。未取到块返回 TIMEOUT。
// 超时不表示采集失败；必须同时检查 get_status 的错误，故障后不可继续当作完整音频。
esp_err_t audio_capture_read(audio_capture_handle_t handle, audio_capture_block_t *out,
                              uint32_t timeout_ms);

// 可跨任务查询；done=true 表示错误和停止状态已经发布，随后队列只减不增。
audio_capture_status_t audio_capture_get_status(audio_capture_handle_t handle);
// 诊断专用：扫描采集任务栈的剩余空间，不应放入高频轮询。
size_t audio_capture_stack_free(audio_capture_handle_t handle);

// 仅 done 时释放，不能与任何其它接口并发；不销毁借用的麦克风。
esp_err_t audio_capture_destroy(audio_capture_handle_t handle);

#ifdef __cplusplus
}
#endif
