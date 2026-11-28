#pragma once

#include "driver/i2c_master.h"

// 仅板级实现使用；创建前进行线路恢复，不对上层额外抽象 I2C 主机。
esp_err_t board_i2c_create(i2c_master_bus_handle_t *out_bus);
i2c_master_bus_handle_t board_i2c_bus(void);
esp_err_t board_set_speaker_power(bool on);
esp_err_t board_set_usb_camera_mux(bool camera);
void board_clock_init(i2c_master_bus_handle_t bus);
void board_battery_init(i2c_master_bus_handle_t bus);
