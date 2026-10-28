#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PCM_OPUS_SAMPLE_RATE = 16000,
    PCM_OPUS_FRAME_SAMPLES = 320,
    PCM_OPUS_BITRATE = 24000,
};

typedef struct pcm_opus_encoder *pcm_opus_encoder_handle_t;
typedef struct {
    uint64_t real_samples;
    uint64_t encoded_samples;
    uint32_t delay_samples; // 输入采样率下的编码延迟，不涉及容器时间基。
} pcm_opus_encoder_info_t;

// 同步交付一包裸 Opus；packet 仅在回调期间有效。samples 是累计编码样本数。
typedef esp_err_t (*pcm_opus_packet_fn)(void *context, const uint8_t *packet,
                                       size_t bytes, uint64_t samples);

// 固定单声道、16 kHz、24 kbps CBR；无文件、Ogg 或硬件依赖。
esp_err_t pcm_opus_encoder_create(pcm_opus_packet_fn output, void *context,
                                  pcm_opus_encoder_handle_t *out);
esp_err_t pcm_opus_encoder_write(pcm_opus_encoder_handle_t encoder, const int16_t *pcm, size_t count);
// 补齐尾部以输出全部真实采样；调用者用 real_samples + delay_samples 裁剪容器尾部。
// finish 后不得继续写入；输出错误会锁存，后续调用均返回首个错误。
esp_err_t pcm_opus_encoder_finish(pcm_opus_encoder_handle_t encoder);
pcm_opus_encoder_info_t pcm_opus_encoder_info(pcm_opus_encoder_handle_t encoder);
void pcm_opus_encoder_destroy(pcm_opus_encoder_handle_t encoder);

#ifdef __cplusplus
}
#endif
