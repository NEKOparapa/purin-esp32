#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ogg_opus_muxer *ogg_opus_muxer_handle_t;
typedef esp_err_t (*ogg_bytes_write_fn)(void *context, const uint8_t *data, size_t bytes);
typedef struct {
    uint32_t serial;
    uint32_t input_sample_rate;
    uint16_t pre_skip; // Ogg Opus 统一使用 48 kHz 时间基。
    uint8_t channels;
} ogg_opus_muxer_config_t;

// 仅封装已编码的 Opus 包；无编码器和文件依赖，字节可写文件、内存或其它目标。
// create 写出独立的 Head/Tags 页；所有回调同步执行，失败锁存。
esp_err_t ogg_opus_muxer_create(const ogg_opus_muxer_config_t *config,
                               ogg_bytes_write_fn output, void *context,
                               ogg_opus_muxer_handle_t *out);
esp_err_t ogg_opus_muxer_packet(ogg_opus_muxer_handle_t muxer, const uint8_t *packet,
                               size_t bytes, uint64_t granule_48k);
esp_err_t ogg_opus_muxer_flush(ogg_opus_muxer_handle_t muxer);
// 保留尾包的页写 EOS，并裁剪补零；finish 后不允许继续写入。
esp_err_t ogg_opus_muxer_finish(ogg_opus_muxer_handle_t muxer, uint64_t final_granule_48k);
uint64_t ogg_opus_muxer_bytes(ogg_opus_muxer_handle_t muxer);
void ogg_opus_muxer_destroy(ogg_opus_muxer_handle_t muxer);

#ifdef __cplusplus
}
#endif
