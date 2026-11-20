#include "board.h"

#include <string.h>
#include <stdatomic.h>
#include "board_internal.h"
#include "board_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_audio";
static i2s_microphone_handle_t board_microphone;
static bool audio_uart_owned;
static i2s_chan_handle_t speaker;
static bool speaker_running;
static atomic_flag audio_owned = ATOMIC_FLAG_INIT;

bool board_audio_reserve(void) { return !atomic_flag_test_and_set(&audio_owned); }
void board_audio_release(void) { atomic_flag_clear(&audio_owned); }

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

static esp_err_t prepare_codec(void)
{
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board power init");
    ESP_RETURN_ON_ERROR(configure_uart(), TAG, "audio UART init");
    // The external codec supplies I2S clocks. Reapply its mode after a reset or failed session.
    audio_delay_ms(BOARD_BT_AUDIO_POWER_WAIT_MS);
    ESP_RETURN_ON_ERROR(uart_flush_input(BOARD_BT_AUDIO_UART_PORT), TAG, "audio UART flush");
    ESP_RETURN_ON_ERROR(send_mode_command("AT+RX=2\r\n"), TAG, "audio RX mode");
    audio_delay_ms(BOARD_BT_AUDIO_MODE_WAIT_MS);
    esp_err_t err = send_mode_command("AT+MODE=1\r\n");
    ESP_RETURN_ON_ERROR(err, TAG, "audio codec mode");
    return ESP_OK;
}

esp_err_t board_microphone_init(i2s_microphone_handle_t *microphone)
{
    ESP_RETURN_ON_FALSE(microphone, ESP_ERR_INVALID_ARG, TAG, "NULL microphone output");
    *microphone = NULL;
    ESP_RETURN_ON_ERROR(board_speaker_stop(), TAG, "release speaker");
    ESP_RETURN_ON_ERROR(board_microphone_deinit(), TAG, "release previous microphone");
    ESP_RETURN_ON_ERROR(prepare_codec(), TAG, "prepare codec");
    const i2s_microphone_config_t config = {
        .port = BOARD_AUDIO_I2S_PORT,
        .sample_rate_hz = BOARD_AUDIO_SAMPLE_RATE_HZ,
        .bclk_gpio = BOARD_AUDIO_BCLK_GPIO,
        .ws_gpio = BOARD_AUDIO_MIC_WS_GPIO,
        .din_gpio = BOARD_AUDIO_MIC_DIN_GPIO,
    };
    ESP_RETURN_ON_ERROR(i2s_microphone_create(&config, &board_microphone), TAG, "I2S RX");
    *microphone = board_microphone;
    ESP_LOGI(TAG, "Stereo microphone ready: I2S slave %u Hz, BCLK=%d WS=%d DIN=%d, PA remains off",
             (unsigned)config.sample_rate_hz, config.bclk_gpio, config.ws_gpio, config.din_gpio);
    return ESP_OK;
}

esp_err_t board_microphone_deinit(void)
{
    if (!board_microphone) return ESP_OK;
    esp_err_t err = i2s_microphone_destroy(board_microphone);
    if (err == ESP_OK) board_microphone = NULL;
    return err;
}

uint8_t board_microphone_pcm_shift(void)
{
    return BOARD_AUDIO_PCM_SHIFT;
}

esp_err_t board_speaker_start(void)
{
    ESP_RETURN_ON_ERROR(board_speaker_stop(), TAG, "release previous speaker");
    ESP_RETURN_ON_ERROR(board_microphone_deinit(), TAG, "release microphone");
    ESP_RETURN_ON_ERROR(prepare_codec(), TAG, "prepare codec");

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_PORT, I2S_ROLE_SLAVE);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 240;
    channel.auto_clear_after_cb = true;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_AUDIO_SPK_BCLK_GPIO,
            .ws = BOARD_AUDIO_MIC_WS_GPIO,
            .dout = BOARD_AUDIO_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &speaker, NULL), TAG, "allocate speaker TX");
    esp_err_t ret;
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(speaker, &standard), fail, TAG, "initialize speaker TX");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(speaker), fail, TAG, "enable speaker TX");
    speaker_running = true;
    ESP_GOTO_ON_ERROR(board_set_speaker_power(true), fail, TAG, "enable speaker power");
    return ESP_OK;

fail:
    (void)board_speaker_stop();
    return ret;
}

esp_err_t board_speaker_write(const int32_t *stereo, size_t frames)
{
    size_t bytes = frames * 2 * sizeof(*stereo), written = 0;
    esp_err_t err = i2s_channel_write(speaker, stereo, bytes, &written, 400);
    return err == ESP_OK && written != bytes ? ESP_ERR_TIMEOUT : err;
}

esp_err_t board_speaker_stop(void)
{
    if (!speaker) return ESP_OK;
    esp_err_t power = ESP_OK;
    if (speaker_running) {
        power = board_set_speaker_power(false);
        ESP_RETURN_ON_ERROR(i2s_channel_disable(speaker), TAG, "disable speaker TX");
        speaker_running = false;
    }
    esp_err_t err = i2s_del_channel(speaker);
    if (err == ESP_OK) speaker = NULL;
    return err == ESP_OK ? power : err;
}
