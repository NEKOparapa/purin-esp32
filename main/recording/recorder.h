#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    RECORDER_IDLE,
    RECORDER_STARTING,
    RECORDER_RECORDING,
    RECORDER_STOPPING,
    RECORDER_ERROR,
} recorder_state_t;

typedef enum {
    RECORDER_ERROR_NONE,
    RECORDER_ERROR_MEMORY,
    RECORDER_ERROR_SD,
    RECORDER_ERROR_STORAGE,
    RECORDER_ERROR_CARD_FULL,
    RECORDER_ERROR_MICROPHONE,
    RECORDER_ERROR_NO_AUDIO,
    RECORDER_ERROR_OVERRUN,
    RECORDER_ERROR_ENCODER,
} recorder_error_t;

typedef struct {
    recorder_state_t state;
    recorder_error_t error;
    uint32_t saved_segments; // 音频与索引均同步成功的分段数。
    uint64_t samples;       // 已交给编码器的真实样本，包含尚未提交的当前分段。
} recorder_status_t;

// 初始化一次，任务及缓冲区持续到重启；失败不影响主页使用。
esp_err_t recorder_init(void);
// 非阻塞；仅 IDLE/ERROR 可开始，仅 RECORDING 可请求停止。
esp_err_t recorder_start(void);
esp_err_t recorder_stop(void);
// 从本次录音起点取前 15/30/60/120 分钟；不足时上传确认时已经采集的全部音频。
// 非阻塞，录音任务先保存快照，上传任务只读取完成文件。
esp_err_t recorder_upload(uint32_t minutes);
// 返回受保护的状态快照；不暴露内部任务或文件句柄。
recorder_status_t recorder_get_status(void);
const char *recorder_error_text(recorder_error_t error);
