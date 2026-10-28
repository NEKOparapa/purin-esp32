#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 先检查原生屏区，再交换坐标轴和镜像，避免无符号下溢。
 * 无效点不修改输出；盖板虚拟键不能钳位成屏幕边缘点击。
 * 宽高均表示像素数量，由使用者传入，不依赖特定板配置。
 */
static inline bool touch_transform_point(uint16_t raw_x, uint16_t raw_y,
                                          uint16_t native_width, uint16_t native_height,
                                          bool swap_xy, bool mirror_x, bool mirror_y,
                                          uint16_t *x, uint16_t *y)
{
    if (!x || !y || raw_x >= native_width || raw_y >= native_height) {
        return false;
    }
    uint16_t tx = swap_xy ? raw_y : raw_x;
    uint16_t ty = swap_xy ? raw_x : raw_y;
    uint16_t width = swap_xy ? native_height : native_width;
    uint16_t height = swap_xy ? native_width : native_height;
    *x = mirror_x ? width - 1 - tx : tx;
    *y = mirror_y ? height - 1 - ty : ty;
    return true;
}

/* 原生竖屏转到面板横屏：x=raw_y，y=native_width-1-raw_x。 */
static inline bool touch_native_to_landscape(uint16_t raw_x, uint16_t raw_y,
                                             uint16_t native_width, uint16_t native_height,
                                             uint16_t *x, uint16_t *y)
{
    return touch_transform_point(raw_x, raw_y, native_width, native_height,
                                 true, false, true, x, y);
}
