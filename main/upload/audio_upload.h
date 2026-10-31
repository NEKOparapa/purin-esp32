#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    AUDIO_UPLOAD_IDLE,
    AUDIO_UPLOAD_PREPARING,
    AUDIO_UPLOAD_CONNECTING,
    AUDIO_UPLOAD_SENDING,
    AUDIO_UPLOAD_ACCEPTED,
    AUDIO_UPLOAD_FAILED,
    AUDIO_UPLOAD_NOT_CONFIGURED,
} audio_upload_state_t;

typedef struct {
    audio_upload_state_t state;
    uint32_t sent_segments;
} audio_upload_status_t;

typedef struct {
    char root[128];
    char recording_id[64];
    uint32_t session_id;
    uint32_t window_minutes;
    uint64_t samples;
    uint64_t elapsed_samples;
    int64_t captured_at_us;
} audio_upload_snapshot_t;

// 无网络活动；只创建队列和低优先级上传任务。
esp_err_t audio_upload_init(void);
audio_upload_status_t audio_upload_get_status(void);
bool audio_upload_busy(void);
// 在 UI 请求时预留唯一上传槽；未配置时明确返回错误。
esp_err_t audio_upload_prepare(void);
// 已提交且不可变的录音快照；录音任务保证完成上传前不卸载文件系统。
esp_err_t audio_upload_submit(const audio_upload_snapshot_t *snapshot);
void audio_upload_fail(void);
const char *audio_upload_status_text(audio_upload_state_t state);
