#pragma once

#include <stdint.h>
#include <limits.h>

// 纯 PCM 运算：在宽整数中先等权平均，再缩放和限幅，避免逐路削顶失真。
static inline int16_t audio_pcm_mix_sample(int32_t left, int32_t right, unsigned shift)
{
    if (shift > 31) {
        return 0;
    }
    const int64_t average = ((int64_t)left + right) / 2;
    const int64_t scaled = average / ((int64_t)1 << shift);
    return scaled > INT16_MAX ? INT16_MAX : scaled < INT16_MIN ? INT16_MIN : (int16_t)scaled;
}
