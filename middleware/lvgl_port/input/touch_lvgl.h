#pragma once

#include "cst816s_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct touch_lvgl *touch_lvgl_handle_t;

typedef struct {
    // 均由 LVGL UI 任务执行，可直接修改控件；取消手势不会触发 on_release。
    void (*on_release)(uint16_t raw_x, uint16_t raw_y, uint16_t x, uint16_t y);
    void (*on_status)(bool available);
} touch_lvgl_callbacks_t;

typedef struct {
    cst816s_touch_handle_t source; // 借用驱动；适配器为其事件队列的唯一消费者。
    lv_display_t *display;
    uint16_t native_width;         // 必须与 source 创建配置相同。
    uint16_t native_height;
    bool swap_xy;                 // 先交换坐标轴，再按交换后的宽高镜像。
    bool mirror_x;
    bool mirror_y;
    touch_lvgl_callbacks_t callbacks;
} touch_lvgl_config_t;

// LVGL 初始化后，在 UI 任务中调用；旋转后的尺寸必须与 display 逻辑分辨率一致。
// 组件内部输入适配不直接访问 GPIO/I2C，不执行板级复位，所有 LVGL API 由 UI 任务调用。
esp_err_t touch_lvgl_create(const touch_lvgl_config_t *config, touch_lvgl_handle_t *out_adapter);

// 在 UI 任务中删除适配器，再由调用者删除 source。不可在其事件回调内部删除。
void touch_lvgl_delete(touch_lvgl_handle_t adapter);

#ifdef __cplusplus
}
#endif
