#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    EPAPER_UI_WIDTH = 480,
    EPAPER_UI_HEIGHT = 800,
    EPAPER_NATIVE_WIDTH = 800,
    EPAPER_NATIVE_HEIGHT = 480,
    EPAPER_FRAME_BYTES = EPAPER_NATIVE_WIDTH * EPAPER_NATIVE_HEIGHT / 8,
};

/* 竖屏 I1 画布转成 SSD1677 的原生横向帧：
 * UI (x, y) -> 面板 (y, 479 - x)。两侧均为 MSB-first，1=白、0=黑。
 * src 已跳过 LVGL 的 8 字节调色板，stride 可包含行尾填充。
 * 调用者提供至少 stride*800 字节源和 48000 字节目标，二者不得重叠。
 */
static inline bool epaper_frame_portrait_to_native(uint8_t *dst, const uint8_t *src,
                                                   size_t stride)
{
    if (!dst || !src || stride < EPAPER_UI_WIDTH / 8 ||
        stride > SIZE_MAX / EPAPER_UI_HEIGHT) {
        return false;
    }
    // 每次生成一个完整目标字节，避免对 PSRAM 目标逐像素读改写。
    for (unsigned panel_y = 0; panel_y < EPAPER_NATIVE_HEIGHT; ++panel_y) {
        const unsigned ui_x = EPAPER_UI_WIDTH - 1 - panel_y;
        const size_t source_byte = ui_x / 8;
        const uint8_t source_mask = (uint8_t)(0x80U >> (ui_x & 7U));
        for (unsigned byte_x = 0; byte_x < EPAPER_NATIVE_WIDTH / 8; ++byte_x) {
            uint8_t packed = 0;
            for (unsigned bit = 0; bit < 8; ++bit) {
                const size_t ui_y = byte_x * 8 + bit;
                if (src[ui_y * stride + source_byte] & source_mask) {
                    packed |= (uint8_t)(0x80U >> bit);
                }
            }
            dst[panel_y * (EPAPER_NATIVE_WIDTH / 8) + byte_x] = packed;
        }
    }
    return true;
}
