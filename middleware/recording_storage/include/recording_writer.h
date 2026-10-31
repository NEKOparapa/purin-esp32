#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RECORDING_WRITER_SAMPLE_RATE = 16000 };

typedef struct recording_writer *recording_writer_handle_t;
typedef enum {
    RECORDING_WRITE_OK,
    RECORDING_WRITE_INVALID_ARGUMENT,
    RECORDING_WRITE_NO_MEMORY,
    RECORDING_WRITE_STORAGE_ERROR,
    RECORDING_WRITE_CARD_FULL,
    RECORDING_WRITE_ENCODER_ERROR,
} recording_write_result_t;

typedef struct {
    uint32_t session_id;
    uint32_t saved_segments; // 音频和索引均同步成功的分段。
    uint64_t samples;       // 已接受的真实 PCM 样本，含当前未完成段。
} recording_writer_stats_t;

/* 同一句柄全部接口串行调用，无任务、按键、板级或 SD 驱动依赖。
 * root 为已挂载的文件系统路径；调用方保证路径在 close 前有效。
 * 接收 16 kHz 单声道 PCM16，输出 24 kbps Ogg Opus。
 * segment_samples 决定每段真实样本数，例如 16000*300 表示五分钟。
 * 独立创建递增会话目录；音频文件在首批 PCM 到达时才创建。
 */
recording_write_result_t recording_writer_open(const char *root, uint64_t segment_samples,
                                               recording_writer_handle_t *out);
// 可跨多个分段；故障锁存，后续调用不再提交音频或索引。
recording_write_result_t recording_writer_write(recording_writer_handle_t writer,
                                                const int16_t *pcm, size_t count);
// 调用方定期调用（当前应用每秒一次）；仅同步已有音频，不创建空文件。
recording_write_result_t recording_writer_sync(recording_writer_handle_t writer);
// 提交当前末段但保持会话打开。后续仍按会话起点的 segment_samples 边界分段。
// 同 write 一样由唯一录音任务调用，成功后已保存文件均不可变。
recording_write_result_t recording_writer_checkpoint(recording_writer_handle_t writer);
recording_writer_stats_t recording_writer_get_stats(recording_writer_handle_t writer);
// 正常保存末段，关闭索引并释放句柄；失败也置 NULL，返回最终统计。
recording_write_result_t recording_writer_finish(recording_writer_handle_t *writer,
                                                 recording_writer_stats_t *stats);
// 不提交当前分段；保留 .part / 未提交文件，关闭并释放，始终置 NULL。
recording_write_result_t recording_writer_abort(recording_writer_handle_t *writer,
                                                recording_writer_stats_t *stats);

#ifdef __cplusplus
}
#endif
