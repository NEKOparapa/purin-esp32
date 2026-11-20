#include "board.h"

#include <string.h>
#include <stdatomic.h>
#include "board_config.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "sdcard.h"

static const char *TAG = "board_sdcard";
static sdcard_handle_t board_card;
static atomic_flag card_in_use = ATOMIC_FLAG_INIT;

static esp_err_t mount_card(board_sdcard_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "NULL card info");
    memset(info, 0, sizeof(*info));
    ESP_RETURN_ON_FALSE(!board_card, ESP_ERR_INVALID_STATE, TAG, "card already mounted");
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board power init");
    const sdcard_config_t config = {
        .mount_point = BOARD_SD_MOUNT_POINT,
        .clk_gpio = BOARD_SDMMC_CLK_GPIO,
        .cmd_gpio = BOARD_SDMMC_CMD_GPIO,
        .d0_gpio = BOARD_SDMMC_D0_GPIO,
        .dat3_gpio = BOARD_SDMMC_DAT3_GPIO,
        .max_freq_khz = BOARD_SDMMC_MAX_FREQ_KHZ,
        .max_files = 4,
    };
    ESP_RETURN_ON_ERROR(sdcard_mount(&config, &board_card), TAG, "mount SD card");
    sdcard_info_t card_info;
    esp_err_t err = sdcard_get_info(board_card, &card_info);
    if (err != ESP_OK) {
        esp_err_t cleanup_err = sdcard_unmount(board_card);
        board_card = NULL;
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG, "Card info cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
        return err;
    }
    memcpy(info->name, card_info.name, sizeof(info->name));
    info->capacity_bytes = card_info.capacity_bytes;
    info->sector_size = card_info.sector_size;
    return ESP_OK;
}

esp_err_t board_sdcard_mount(board_sdcard_info_t *info)
{
    if (atomic_flag_test_and_set(&card_in_use)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = mount_card(info);
    if (err != ESP_OK) atomic_flag_clear(&card_in_use);
    return err;
}

esp_err_t board_sdcard_unmount(void)
{
    ESP_RETURN_ON_FALSE(board_card, ESP_ERR_INVALID_STATE, TAG, "card not mounted");
    esp_err_t err = sdcard_unmount(board_card);
    board_card = NULL; // 底层 unmount 消耗有效句柄；即使报错也不可重用。
    atomic_flag_clear(&card_in_use);
    return err;
}

const char *board_sdcard_path(void)
{
    return board_card ? sdcard_mount_path(board_card) : NULL;
}

esp_err_t board_sdcard_read_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!total_bytes || !free_bytes) return ESP_ERR_INVALID_ARG;
    board_sdcard_info_t info;
    esp_err_t err = board_sdcard_mount(&info);
    if (err != ESP_OK) return err;

    err = esp_vfs_fat_info(board_sdcard_path(), total_bytes, free_bytes);
    esp_err_t unmount_err = board_sdcard_unmount();
    if (err != ESP_OK) return err;
    if (*free_bytes > *total_bytes) return ESP_ERR_INVALID_RESPONSE;
    return unmount_err;
}
