#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cst816s_touch *cst816s_touch_handle_t;

typedef struct {
    i2c_master_bus_handle_t bus; // 借用已初始化的共享总线，驱动不创建或释放它。
    uint16_t i2c_address;        // CST816S 通常为 0x15。
    uint32_t i2c_speed_hz;
    gpio_num_t interrupt_gpio;  // 独占该引脚的下降沿中断，内部上拉。
    uint16_t native_width;      // 原生有效区域的像素数量；不包含盖板虚拟键。
    uint16_t native_height;
} cst816s_touch_config_t;

typedef struct {
    uint16_t raw_x, raw_y; // 松手/取消时保留最近有效触点，不做旋转或镜像。
    bool down;
    bool cancel;          // 通信失败、越界或队列溢出；上层应取消手势，不能触发点击。
    bool ready;           // 最近一次通信是否成功，不等同于当前是否按下。
} cst816s_touch_sample_t;

// 调用者负责上电和硬件复位，随后创建驱动；不依赖板级配置或 LVGL。
// ESP_OK 仅表示采样通道启动。CST816S 休眠时允许探测失败，下次 IRQ 自动重试。
// 内部采样任务只在 IRQ 或按住期间读取芯片，队列供一个消费者顺序读取。
esp_err_t cst816s_touch_create(const cst816s_touch_config_t *config,
                              cst816s_touch_handle_t *out_touch);

// 非阻塞读取下一个状态变化；没有事件返回 false，不修改 sample。
bool cst816s_touch_read(cst816s_touch_handle_t touch, cst816s_touch_sample_t *sample);
bool cst816s_touch_has_pending(cst816s_touch_handle_t touch);

// 先移除消费者（例如 touch_lvgl_delete），再删除驱动。不得与 read/create/delete 并发。
// 停止任务后释放设备与队列，保留共享总线和全局 GPIO ISR 服务。
// 若 I2C 事务未能在 5 秒内结束则返回 ESP_ERR_TIMEOUT，句柄仍有效，可稍后重试删除。
esp_err_t cst816s_touch_delete(cst816s_touch_handle_t touch);

#ifdef __cplusplus
}
#endif
