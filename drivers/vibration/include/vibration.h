#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vibration_device *vibration_handle_t;

typedef struct {
    gpio_num_t gpio;             // 马达驱动电路的使能引脚，必须支持输出。
    bool active_high;            // true：高电平震动；false：低电平震动。
    uint32_t default_pulse_ms;    // pulse(0) 使用此时长，必须大于零。
    uint32_t max_pulse_ms;        // 单次脉冲上限，不小于 default_pulse_ms。
} vibration_config_t;

// 初始化后保持关闭。每个 GPIO 只能创建一个实例，调用者独占此引脚。
// 不依赖板级配置、LVGL 或电源控制；调用者负责马达驱动电路的供电。
// 句柄、互斥锁与定时器常驻到重启。IDF 5.5 的 timer stop/delete 不等待已分发
// 回调，因此本组件不提供运行中释放接口；不再使用时调用 stop 即可关闭输出。
esp_err_t vibration_create(const vibration_config_t *config, vibration_handle_t *out_handle);

// 非阻塞脉冲：0 使用默认时长；超过上限返回 INVALID_ARG，不改变当前脉冲。
// 再次调用从当前时刻重新计时，不累加时长；定时器异常时尝试立即关闭输出。
// 自动关断由 ESP_TIMER_TASK 执行，正常调度下在到期时关断，不是硬实时波形。
esp_err_t vibration_pulse(vibration_handle_t handle, uint32_t duration_ms);

// 取消计时并立即关闭；可重复调用。若 GPIO 写入报错，可再次调用重试。
esp_err_t vibration_stop(vibration_handle_t handle);

// 返回最近成功写入的输出状态；不检测马达连接或实际是否产生震动。
// pulse/stop/is_active 可由多个普通任务调用，内部互斥；禁止 ISR 调用。
bool vibration_is_active(vibration_handle_t handle);

#ifdef __cplusplus
}
#endif
