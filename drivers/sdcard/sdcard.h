#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sdcard_device *sdcard_handle_t;

typedef struct {
    // VFS 挂载点：以 / 开头、末尾不带 /，最长 13 字符，例如 /sdcard。
    // 驱动复制此字符串，配置结构可在 mount 返回后销毁。
    const char *mount_point;
    gpio_num_t clk_gpio;
    gpio_num_t cmd_gpio;
    gpio_num_t d0_gpio;
    // 1-bit 模式不驱动 DAT3。若连接此脚，只设为输入上拉；不连接填 GPIO_NUM_NC。
    gpio_num_t dat3_gpio;
    int max_freq_khz; // 0 使用 20 MHz；非零范围 400～40000 kHz。
    int max_files;    // 同时打开的文件数，0 使用 5，范围 1～32。
} sdcard_config_t;

typedef struct {
    char name[8];
    uint64_t capacity_bytes;
    uint32_t sector_size;
    bool is_mmc;
} sdcard_info_t;

// 挂载已格式化的 FAT 卡，固定 SDMMC 1-bit；绝不格式化或重建分区。
// 调用前由板级代码供电。本模块独占 SDMMC 主机，只支持一张卡；若主机
// 已被其他驱动初始化则返回 ESP_ERR_INVALID_STATE，不接管或释放他人资源。
// 生命周期函数须串行调用，挂载点不能同时交给其它 VFS/USB MSC 使用。
// 成功后可用 fopen/open 等标准 VFS 接口读写；失败时 *out_handle = NULL。
esp_err_t sdcard_mount(const sdcard_config_t *config, sdcard_handle_t *out_handle);

// 先停止所有文件访问并关闭文件，再卸载；禁止与文件读写/查询并发调用。
// 传入有效句柄后，无论返回值如何，此句柄均失效，调用者应将其置 NULL。
// 无热插拔检测；应在卸载后拔卡。DAT3 输入上拉保持，避免下一次上电进 SPI 模式。
esp_err_t sdcard_unmount(sdcard_handle_t handle);

// 返回挂载时读到的卡信息，不访问卡，不代表当前物理插入状态。
esp_err_t sdcard_get_info(sdcard_handle_t handle, sdcard_info_t *out_info);

// 返回驱动持有的路径字符串，卸载后失效；无效句柄返回 NULL。
const char *sdcard_mount_path(sdcard_handle_t handle);

#ifdef __cplusplus
}
#endif
