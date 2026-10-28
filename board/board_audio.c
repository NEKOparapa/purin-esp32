#include "board.h"

#include <string.h>
#include "board_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_audio";
static i2s_microphone_handle_t board_microphone;
static bool audio_uart_owned;

static void audio_delay_ms(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t configure_uart(void)
{
    if (audio_uart_owned) {
        return ESP_OK;
    }
    // 不接管其它模块的 UART，初始化失败也只清理自己创建的驱动。
    ESP_RETURN_ON_FALSE(!uart_is_driver_installed(BOARD_BT_AUDIO_UART_PORT),
                        ESP_ERR_INVALID_STATE, TAG, "audio UART already in use");
    const uart_config_t config = {
        .baud_rate = BOARD_BT_AUDIO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(BOARD_BT_AUDIO_UART_PORT, 1024, 0, 0, NULL, 0),
                        TAG, "audio UART driver");
    esp_err_t err = uart_param_config(BOARD_BT_AUDIO_UART_PORT, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(BOARD_BT_AUDIO_UART_PORT, BOARD_BT_AUDIO_TX_GPIO,
                           BOARD_BT_AUDIO_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(BOARD_BT_AUDIO_UART_PORT);
        return err;
    }
    audio_uart_owned = true;
    return ESP_OK;
}

static esp_err_t send_mode_command(const char *command)
{
    const size_t length = strlen(command);
    if (uart_write_bytes(BOARD_BT_AUDIO_UART_PORT, command, length) != (int)length) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(BOARD_BT_AUDIO_UART_PORT, pdMS_TO_TICKS(100));
}

esp_err_t board_microphone_init(i2s_microphone_handle_t *microphone)
{
    ESP_RETURN_ON_FALSE(microphone, ESP_ERR_INVALID_ARG, TAG, "NULL microphone output");
    *microphone = NULL;
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board power init");
    ESP_RETURN_ON_ERROR(configure_uart(), TAG, "audio UART init");
    // 外置芯片是 I2S 时钟主机。每次录音准备都重发原板模式指令，允许
    // 上次无时钟/模块复位后重试；调用时采集必须已停止。UART 与 I2S 资源复用。
    // 这里只配置音频输入，不开启 TCA9555 的 PA 输出。
    audio_delay_ms(BOARD_BT_AUDIO_POWER_WAIT_MS);
    ESP_RETURN_ON_ERROR(uart_flush_input(BOARD_BT_AUDIO_UART_PORT), TAG, "audio UART flush");
    ESP_RETURN_ON_ERROR(send_mode_command("AT+RX=2\r\n"), TAG, "audio RX mode");
    audio_delay_ms(BOARD_BT_AUDIO_MODE_WAIT_MS);
    ESP_RETURN_ON_ERROR(send_mode_command("AT+MODE=1\r\n"), TAG, "audio codec mode");
    if (board_microphone) {
        *microphone = board_microphone;
        return ESP_OK;
    }
    const i2s_microphone_config_t config = {
        .port = BOARD_AUDIO_I2S_PORT,
        .sample_rate_hz = BOARD_AUDIO_SAMPLE_RATE_HZ,
        .bclk_gpio = BOARD_AUDIO_BCLK_GPIO,
        .ws_gpio = BOARD_AUDIO_MIC_WS_GPIO,
        .din_gpio = BOARD_AUDIO_MIC_DIN_GPIO,
    };
    ESP_RETURN_ON_ERROR(i2s_microphone_create(&config, &board_microphone), TAG, "I2S microphone");
    *microphone = board_microphone;
    ESP_LOGI(TAG, "Stereo microphone ready: I2S slave %u Hz, BCLK=%d WS=%d DIN=%d, PA remains off",
             (unsigned)config.sample_rate_hz, config.bclk_gpio, config.ws_gpio, config.din_gpio);
    return ESP_OK;
}

uint8_t board_microphone_pcm_shift(void)
{
    return BOARD_AUDIO_PCM_SHIFT;
}
