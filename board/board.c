// 板级上电顺序来自 Metalio-E-INK4-main，来源见 drivers/ssd1677/NOTICE.md。
#include "board.h"

#include "board_config.h"
#include "board_internal.h"
#include "esp_check.h"
#include "tca9555.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t board_bus;
static tca9555_handle_t expander;
static cst816s_touch_handle_t board_touch;
static bool initialized;

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

esp_err_t board_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    // 中途失败时复用已分配资源，允许调用方修复供电后重试。
    if (!board_bus) {
        ESP_RETURN_ON_ERROR(board_i2c_create(&board_bus), TAG, "shared I2C");
    }
    esp_err_t probe = ESP_ERR_NOT_FOUND;
    for (int attempt = 0; attempt < 10; ++attempt) {
        delay_ms(20);
        probe = i2c_master_probe(board_bus, BOARD_IO_EXPANDER_ADDRESS, 50);
        if (probe == ESP_OK) {
            break;
        }
        ESP_RETURN_ON_ERROR(i2c_master_bus_reset(board_bus), TAG, "I2C recovery");
    }
    ESP_RETURN_ON_ERROR(probe, TAG, "TCA9555 missing at 0x%02x; check power / SDA%d / SCL%d",
                        BOARD_IO_EXPANDER_ADDRESS, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    if (!expander) {
        const tca9555_config_t config = {
            .address = BOARD_IO_EXPANDER_ADDRESS,
            .clock_hz = BOARD_IO_EXPANDER_I2C_HZ,
            .timeout_ms = 200,
        };
        ESP_RETURN_ON_ERROR(tca9555_create(board_bus, &config, &expander), TAG, "TCA9555 device");
    }
    const uint16_t high = BOARD_IO_MAIN_POWER_MASK | BOARD_IO_USB_MUX_MASK |
                          BOARD_IO_POWER_KEY_MASK | BOARD_IO_TOUCH_RESET_MASK;
    const uint16_t low = BOARD_IO_PA_ENABLE_MASK | BOARD_IO_PA_SWITCH_MASK;
    // 锁存先于方向写入：关机脚保持高、USB 保持调试口，功放关闭。
    ESP_RETURN_ON_ERROR(tca9555_configure_outputs(expander, high | low, high), TAG, "main power outputs");
    delay_ms(20);
    ESP_RETURN_ON_ERROR(tca9555_configure_outputs(expander, BOARD_IO_SCREEN_POWER_MASK,
                                                BOARD_IO_SCREEN_POWER_MASK), TAG, "screen/card power");
    delay_ms(100);
    const gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&boot), TAG, "BOOT button");
    initialized = true;
    ESP_LOGI(TAG, "TCA9555: main/screen power ON, USB=debug, PA=off");
    return ESP_OK;
}

bool board_boot_pressed(void)
{
    return initialized && gpio_get_level(BOARD_BOOT_GPIO) == 0;
}

esp_err_t board_touch_init(cst816s_touch_handle_t *touch, board_touch_orientation_t *orientation)
{
    ESP_RETURN_ON_FALSE(touch && orientation, ESP_ERR_INVALID_ARG, TAG, "NULL touch output");
    *touch = NULL;
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board power init");
    if (!board_touch) {
        ESP_RETURN_ON_ERROR(tca9555_configure_outputs(expander, BOARD_IO_TOUCH_RESET_MASK, 0),
                            TAG, "touch RST low");
        delay_ms(10);
        // 每次读当前值并仅改复位位，不恢复其它端口的旧快照。
        ESP_RETURN_ON_ERROR(tca9555_write_outputs(expander, BOARD_IO_TOUCH_RESET_MASK, BOARD_IO_TOUCH_RESET_MASK),
                            TAG, "touch RST high");
        delay_ms(120);
        const cst816s_touch_config_t config = {
            .bus = board_bus,
            .i2c_address = BOARD_TOUCH_I2C_ADDRESS,
            .i2c_speed_hz = BOARD_TOUCH_I2C_HZ,
            .interrupt_gpio = BOARD_TOUCH_INT_GPIO,
            .native_width = BOARD_TOUCH_NATIVE_WIDTH,
            .native_height = BOARD_TOUCH_NATIVE_HEIGHT,
        };
        ESP_RETURN_ON_ERROR(cst816s_touch_create(&config, &board_touch), TAG, "touch input");
    }
    *touch = board_touch;
    *orientation = (board_touch_orientation_t) {
        .native_width = BOARD_TOUCH_NATIVE_WIDTH,
        .native_height = BOARD_TOUCH_NATIVE_HEIGHT,
        .swap_xy = BOARD_TOUCH_SWAP_XY,
        .mirror_x = BOARD_TOUCH_MIRROR_X,
        .mirror_y = BOARD_TOUCH_MIRROR_Y,
    };
    return ESP_OK;
}
