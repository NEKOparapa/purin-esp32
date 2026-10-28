#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OGG_OPUS_SAMPLE_RATE = 16000,
    OGG_OPUS_FRAME_SAMPLES = 320,
    OGG_OPUS_BITRATE = 24000,
};

typedef struct ogg_opus_writer *ogg_opus_handle_t;
typedef struct {
    uint64_t samples; // 真实单声道样本数，不包括编码器延迟和补零。
    uint64_t bytes;   // 完整 Ogg 文件大小。
} ogg_opus_result_t;

// 文件便捷接口：内部组合独立的 pcm_opus_encoder 与 ogg_opus_muxer。
// 需要裸 Opus 包或其它输出目标时，可单独使用这两个模块的公开头文件。
// 文件错误保留首个 errno；编码/容器错误不携带文件 errno。

// 独占创建文件，已有路径返回错误，绝不覆盖。每个文件使用独立编码器。
// 本模块不重命名文件、不管理 SD 生命周期；调用者串行调用全部接口。
esp_err_t ogg_opus_open(const char *path, uint32_t serial, ogg_opus_handle_t *out);
// 接收任意长度的 PCM16；内部按 20 ms 编码，保留不足一帧的样本。
esp_err_t ogg_opus_write(ogg_opus_handle_t handle, const int16_t *pcm, size_t count);
// 写出当前完整编码包并同步文件；不足一帧的 PCM 留待后续写入。
esp_err_t ogg_opus_sync(ogg_opus_handle_t handle);
// 补齐编码器尾部，写 EOS、同步并关闭。无论成功失败均消耗句柄并置 NULL。
// 空音频返回 INVALID_STATE；调用者应在收到首批 PCM 后才创建文件。
esp_err_t ogg_opus_finish(ogg_opus_handle_t *handle, ogg_opus_result_t *out);
// 不写 EOS，关闭并保留不完整文件；无论结果如何均消耗句柄并置 NULL。
esp_err_t ogg_opus_abort(ogg_opus_handle_t *handle);

#ifdef __cplusplus
}
#endif
