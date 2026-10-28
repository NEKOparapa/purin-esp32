#include "board_internal.h"

#include "board_config.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

static const char *TAG = "board_i2c";

static esp_err_t release_clock(gpio_num_t scl)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(scl, 1), TAG, "release SCL");
    // 允许从机短暂拉伸时钟，但不会在损坏或未供电的总线上无限等待。
    for (unsigned wait = 0; wait < 2000; wait += 5) {
        if (gpio_get_level(scl)) {
            return ESP_OK;
        }
        esp_rom_delay_us(5);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t recover_lines(void)
{
    const gpio_num_t sda = BOARD_I2C_SDA_GPIO;
    const gpio_num_t scl = BOARD_I2C_SCL_GPIO;
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(sda) && GPIO_IS_VALID_OUTPUT_GPIO(scl) && sda != scl,
                        ESP_ERR_INVALID_ARG, TAG, "invalid I2C pins");
    // 提前设置高电平锁存，再启用开漏，避免切换模式时产生额外低脉冲。
    ESP_RETURN_ON_ERROR(gpio_set_level(sda, 1), TAG, "release SDA");
    ESP_RETURN_ON_ERROR(gpio_set_level(scl, 1), TAG, "release SCL");
    const gpio_config_t pins = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pins), TAG, "recovery pins");
    ESP_RETURN_ON_ERROR(release_clock(scl), TAG, "SCL held low");
    esp_rom_delay_us(10);
    for (int pulse = 0; pulse < 9 && !gpio_get_level(sda); ++pulse) {
        ESP_RETURN_ON_ERROR(gpio_set_level(scl, 0), TAG, "SCL low");
        esp_rom_delay_us(5);
        ESP_RETURN_ON_ERROR(release_clock(scl), TAG, "SCL held low");
        esp_rom_delay_us(5);
    }
    // STOP：SCL 为高时释放 SDA。
    ESP_RETURN_ON_ERROR(gpio_set_level(scl, 0), TAG, "SCL low");
    ESP_RETURN_ON_ERROR(gpio_set_level(sda, 0), TAG, "SDA low");
    esp_rom_delay_us(5);
    ESP_RETURN_ON_ERROR(release_clock(scl), TAG, "SCL held low");
    esp_rom_delay_us(5);
    ESP_RETURN_ON_ERROR(gpio_set_level(sda, 1), TAG, "release SDA");
    esp_rom_delay_us(5);
    return gpio_get_level(sda) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t board_i2c_create(i2c_master_bus_handle_t *out_bus)
{
    ESP_RETURN_ON_FALSE(out_bus, ESP_ERR_INVALID_ARG, TAG, "NULL output");
    *out_bus = NULL;
    ESP_RETURN_ON_ERROR(recover_lines(), TAG, "line recovery");
    const i2c_master_bus_config_t config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, out_bus);
}
