/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

// @todo fix the hard-coded number here: Should be taken from HAL FIFO config in future versions of esp-idf
#if (CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31 || CONFIG_IDF_TARGET_LINUX)
#define MAX_MPS_IN 4096
#else
// If FIFO is configured for bias IN, we have 608 bytes available.
// However, configuring MPS > 596 causes USB transfer failures,
// because end of the RX FIFO is used internally by the USB-OTG peripheral
#define MAX_MPS_IN 596
#endif
