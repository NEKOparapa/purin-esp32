#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tca9555_device *tca9555_handle_t;

typedef struct {
    uint8_t address;        // 7 位地址：0x20～0x27
    uint32_t clock_hz;      // I2C 速率，例如 100000
    int timeout_ms;         // 每次总线事务的超时，必须大于 0
} tca9555_config_t;

// 借用总线，不拥有总线；每个芯片只创建一个句柄并由多个使用者共享。
// 创建只添加设备，不改变任何端口的方向或电平。
esp_err_t tca9555_create(i2c_master_bus_handle_t bus, const tca9555_config_t *config,
                         tca9555_handle_t *out_device);

// 销毁前须停止使用该句柄的任务；不关闭电源，不删除共享 I2C 总线。
esp_err_t tca9555_delete(tca9555_handle_t device);

// 以下操作可在多个普通任务中并发调用，不可在 ISR 中使用。
// 掩码位 0～7 为 P0，位 8～15 为 P1；仅改变 mask 选择的位。
esp_err_t tca9555_read_inputs(tca9555_handle_t device, uint16_t *values);
esp_err_t tca9555_read_outputs(tca9555_handle_t device, uint16_t *values);
esp_err_t tca9555_write_outputs(tca9555_handle_t device, uint16_t mask, uint16_t values);

// 在同一互斥锁中先写输出锁存、再切换方向，避免使能输出时出现低脉冲。
esp_err_t tca9555_configure_outputs(tca9555_handle_t device, uint16_t mask, uint16_t values);
esp_err_t tca9555_configure_inputs(tca9555_handle_t device, uint16_t mask);

#ifdef __cplusplus
}
#endif
