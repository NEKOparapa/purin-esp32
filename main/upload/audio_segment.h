#pragma once

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    uint32_t sequence;
    uint64_t start;
    uint64_t samples;
    uint64_t bytes;
} audio_segment_t;

// 索引是本固件写入的固定格式，只接收完整提交行和快照范围内的连续分段。
static inline bool audio_segment_parse(const char *line, uint32_t sequence, uint64_t start,
                                        uint64_t end, audio_segment_t *segment)
{
    uint32_t filename;
    uint64_t duration;
    int used = 0;
    int parsed = sscanf(line, "{\"type\":\"segment\",\"sequence\":%" SCNu32
                        ",\"file\":\"%" SCNu32 ".ogg\",\"start_sample\":%" SCNu64
                        ",\"samples\":%" SCNu64 ",\"duration_ms\":%" SCNu64
                        ",\"bytes\":%" SCNu64 "}%n", &segment->sequence, &filename, &segment->start,
                        &segment->samples, &duration, &segment->bytes, &used);
    return parsed == 6 && used > 0 && line[used] == '\n' && line[used + 1] == '\0' &&
           segment->sequence == sequence && filename == sequence && segment->start == start &&
           start <= end && segment->samples && segment->samples <= end - start &&
           segment->samples <= 16000 * 300 && duration == segment->samples * 1000 / 16000 &&
           segment->bytes && segment->bytes <= 4 * 1024 * 1024;
}
