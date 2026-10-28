#pragma once

#include "driver/i2c_master.h"

// 仅板级实现使用；创建前进行线路恢复，不对上层额外抽象 I2C 主机。
esp_err_t board_i2c_create(i2c_master_bus_handle_t *out_bus);
