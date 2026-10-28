#include "sdcard.h"

#include <stdlib.h>
#include <string.h>
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

struct sdcard_device {
    sdmmc_card_t card;
    char mount_point[ESP_VFS_PATH_MAX];
    char drive[3];
    BYTE pdrv;
    int slot;
    bool host_initialized;
    bool slot_initialized;
    bool disk_registered;
    bool vfs_registered;
};

// SDMMC 主机的初始化/释放不是线程安全接口，由调用者串行管理生命周期。
static sdcard_handle_t s_mounted;

static bool valid_config(const sdcard_config_t *config)
{
    if (!config || !config->mount_point) {
        return false;
    }
    size_t length = strnlen(config->mount_point, ESP_VFS_PATH_MAX);
    // IDF 5.5 FAT VFS 内部复制路径时保留两个字符，限制长度避免路径截断。
    if (length < 2 || length > ESP_VFS_PATH_MAX - 2 ||
        config->mount_point[0] != '/' || config->mount_point[length - 1] == '/') {
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config->clk_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->cmd_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->d0_gpio) ||
        (config->dat3_gpio != GPIO_NUM_NC && !GPIO_IS_VALID_GPIO(config->dat3_gpio))) {
        return false;
    }
    if (config->clk_gpio == config->cmd_gpio || config->clk_gpio == config->d0_gpio ||
        config->cmd_gpio == config->d0_gpio ||
        (config->dat3_gpio != GPIO_NUM_NC &&
         (config->dat3_gpio == config->clk_gpio || config->dat3_gpio == config->cmd_gpio ||
          config->dat3_gpio == config->d0_gpio))) {
        return false;
    }
    return (config->max_freq_khz == 0 ||
            (config->max_freq_khz >= 400 && config->max_freq_khz <= 40000)) &&
           config->max_files >= 0 && config->max_files <= 32;
}

static esp_err_t release_resources(sdcard_handle_t device)
{
    esp_err_t result = ESP_OK;
    if (device->vfs_registered) {
        // 即使 f_mount(..., 1) 失败，也可能已注册 FATFS 对象，需要先解除。
        if (f_mount(NULL, device->drive, 0) != FR_OK) {
            result = ESP_FAIL;
        }
        esp_err_t err = esp_vfs_fat_unregister_path(device->mount_point);
        if (result == ESP_OK) {
            result = err;
        }
    }
    if (device->disk_registered) {
        ff_diskio_unregister(device->pdrv);
    }
    if (device->host_initialized) {
        // 正常情况下按 slot 释放，使 IDF 同时更新已初始化槽位计数。
        // 若 init_slot 尚未成功，主机没有有效槽位，直接释放主机资源。
        esp_err_t err = device->slot_initialized ? sdmmc_host_deinit_slot(device->slot)
                                                   : sdmmc_host_deinit();
        if (result == ESP_OK) {
            result = err;
        }
    }
    free(device);
    return result;
}

esp_err_t sdcard_mount(const sdcard_config_t *config, sdcard_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!valid_config(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    sdmmc_host_state_t state;
    esp_err_t err = sdmmc_host_get_state(&state);
    if (err != ESP_OK) {
        return err;
    }
    if (state.host_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    sdcard_handle_t device = calloc(1, sizeof(*device));
    if (!device) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(device->mount_point, config->mount_point, strlen(config->mount_point) + 1);
    device->pdrv = FF_DRV_NOT_USED;
    FATFS *filesystem = NULL;

    if (config->dat3_gpio != GPIO_NUM_NC) {
        const gpio_config_t dat3 = {
            .pin_bit_mask = 1ULL << config->dat3_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&dat3);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    device->slot = host.slot;
    host.flags &= ~(SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_DDR);
    host.flags |= SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = config->max_freq_khz ? config->max_freq_khz : SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = config->clk_gpio;
    slot.cmd = config->cmd_gpio;
    slot.d0 = config->d0_gpio;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
    slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = sdmmc_host_init();
    if (err != ESP_OK) {
        goto fail;
    }
    device->host_initialized = true;
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        goto fail;
    }
    device->slot_initialized = true;
    err = sdmmc_card_init(&host, &device->card);
    if (err != ESP_OK) {
        goto fail;
    }
    err = ff_diskio_get_drive(&device->pdrv);
    if (err != ESP_OK) {
        goto fail;
    }
    ff_diskio_register_sdmmc(device->pdrv, &device->card);
    device->disk_registered = true;
    ff_sdmmc_set_disk_status_check(device->pdrv, true);
    device->drive[0] = '0' + device->pdrv;
    device->drive[1] = ':';
    device->drive[2] = '\0';

    const esp_vfs_fat_conf_t fat_config = {
        .base_path = device->mount_point,
        .fat_drive = device->drive,
        .max_files = config->max_files ? config->max_files : 5,
    };
    err = esp_vfs_fat_register_cfg(&fat_config, &filesystem);
    if (err != ESP_OK) {
        // 重复挂载点会返回 INVALID_STATE；不能释放其它调用者的 VFS。
        goto fail;
    }
    device->vfs_registered = true;
    FRESULT fat_result = f_mount(filesystem, device->drive, 1);
    if (fat_result != FR_OK) {
        ESP_LOGW(TAG, "FAT mount failed (%d); card was not formatted", fat_result);
        err = ESP_FAIL;
        goto fail;
    }
    s_mounted = device;
    *out_handle = device;
    ESP_LOGI(TAG, "Mounted %s in SDMMC 1-bit mode", device->mount_point);
    return ESP_OK;

fail:
    {
        esp_err_t cleanup_err = release_resources(device);
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG, "Mount cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
        return err;
    }
}

esp_err_t sdcard_unmount(sdcard_handle_t handle)
{
    if (!handle || handle != s_mounted) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mounted = NULL;
    return release_resources(handle);
}

esp_err_t sdcard_get_info(sdcard_handle_t handle, sdcard_info_t *out_info)
{
    if (!handle || handle != s_mounted || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));
    // CID 是固定大小数组；同时限制源和目标，保留目标末尾的零终止符。
    size_t name_length = strnlen(handle->card.cid.name, sizeof(handle->card.cid.name));
    if (name_length >= sizeof(out_info->name)) {
        name_length = sizeof(out_info->name) - 1;
    }
    memcpy(out_info->name, handle->card.cid.name, name_length);
    out_info->sector_size = handle->card.csd.sector_size;
    out_info->capacity_bytes = (uint64_t)handle->card.csd.capacity * out_info->sector_size;
    out_info->is_mmc = handle->card.is_mmc;
    return ESP_OK;
}

const char *sdcard_mount_path(sdcard_handle_t handle)
{
    return handle && handle == s_mounted ? handle->mount_point : NULL;
}
