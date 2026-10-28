#include "i2s_microphone.h"

#include <stdbool.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

enum { FRAME_BYTES = 2 * sizeof(int32_t), DMA_FRAMES = 240, DMA_COUNT = 6 };

struct i2s_microphone_device {
    i2s_chan_handle_t rx;
    bool running;
    uint8_t partial[FRAME_BYTES - 1];
    size_t partial_bytes;
    size_t dma_read_offset;
    portMUX_TYPE lock;
    uint32_t received_bytes;
    uint32_t consumed_bytes;
    bool data_lost;
};

static bool IRAM_ATTR on_receive(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    i2s_microphone_handle_t mic = arg;
    portENTER_CRITICAL_ISR(&mic->lock);
    mic->received_bytes += (uint32_t)event->size;
    // IDF 在 RX 队列接近满时可能跳过当前 DMA 尾部，早于 q_ovf 回调。
    // 提前锁存危险积压，不能让这种跳帧伪装为连续录音。无符号差值允许计数回绕。
    if (mic->received_bytes - mic->consumed_bytes >=
        (DMA_COUNT - 2U) * DMA_FRAMES * FRAME_BYTES) {
        mic->data_lost = true;
    }
    portEXIT_CRITICAL_ISR(&mic->lock);
    return false;
}

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    (void)event;
    i2s_microphone_handle_t mic = arg;
    portENTER_CRITICAL_ISR(&mic->lock);
    mic->data_lost = true;
    portEXIT_CRITICAL_ISR(&mic->lock);
    return false;
}

static bool consume_bytes(i2s_microphone_handle_t mic, size_t bytes)
{
    portENTER_CRITICAL(&mic->lock);
    mic->consumed_bytes += (uint32_t)bytes;
    const bool lost = mic->data_lost;
    portEXIT_CRITICAL(&mic->lock);
    return lost;
}

esp_err_t i2s_microphone_create(const i2s_microphone_config_t *config,
                               i2s_microphone_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!config || config->port < I2S_NUM_0 || config->port >= SOC_I2S_NUM ||
        config->sample_rate_hz < 8000 || config->sample_rate_hz > 48000 ||
        !GPIO_IS_VALID_GPIO(config->bclk_gpio) || !GPIO_IS_VALID_GPIO(config->ws_gpio) ||
        !GPIO_IS_VALID_GPIO(config->din_gpio) || config->bclk_gpio == config->ws_gpio ||
        config->bclk_gpio == config->din_gpio || config->ws_gpio == config->din_gpio) {
        return ESP_ERR_INVALID_ARG;
    }
    // ISR 访问的状态固定放内部 RAM，兼容 I2S ISR IRAM-safe 配置。
    i2s_microphone_handle_t mic = heap_caps_calloc(1, sizeof(*mic),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mic) {
        return ESP_ERR_NO_MEM;
    }
    mic->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(config->port, I2S_ROLE_SLAVE);
    channel.dma_desc_num = DMA_COUNT;
    channel.dma_frame_num = DMA_FRAMES;
    esp_err_t err = i2s_new_channel(&channel, NULL, &mic->rx);
    if (err != ESP_OK) {
        goto fail;
    }
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = config->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = config->din_gpio,
        },
    };
    err = i2s_channel_init_std_mode(mic->rx, &standard);
    if (err != ESP_OK) {
        goto fail;
    }
    const i2s_event_callbacks_t callbacks = {
        .on_recv = on_receive,
        .on_recv_q_ovf = on_overflow,
    };
    err = i2s_channel_register_event_callback(mic->rx, &callbacks, mic);
    if (err != ESP_OK) {
        goto fail;
    }
    *out_handle = mic;
    return ESP_OK;

fail:
    if (mic->rx) {
        i2s_del_channel(mic->rx);
    }
    heap_caps_free(mic);
    return err;
}

esp_err_t i2s_microphone_start(i2s_microphone_handle_t mic)
{
    if (!mic) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mic->running) {
        return ESP_ERR_INVALID_STATE;
    }
    mic->partial_bytes = 0;
    mic->dma_read_offset = 0;
    portENTER_CRITICAL(&mic->lock);
    mic->received_bytes = mic->consumed_bytes = 0;
    mic->data_lost = false;
    portEXIT_CRITICAL(&mic->lock);
    esp_err_t err = i2s_channel_enable(mic->rx);
    if (err == ESP_OK) {
        mic->running = true;
    }
    return err;
}

esp_err_t i2s_microphone_read(i2s_microphone_handle_t mic, int32_t *interleaved_lr,
                             size_t capacity_frames, size_t *frames_read, uint32_t timeout_ms)
{
    if (!frames_read) {
        return ESP_ERR_INVALID_ARG;
    }
    *frames_read = 0;
    if (!mic || !interleaved_lr || !capacity_frames || capacity_frames > SIZE_MAX / FRAME_BYTES ||
        timeout_ms == 0 || timeout_ms > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mic->running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (consume_bytes(mic, 0)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *output = (uint8_t *)interleaved_lr;
    size_t total = mic->partial_bytes;
    memcpy(output, mic->partial, total);
    const size_t requested = capacity_frames * FRAME_BYTES;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t err = ESP_OK;
    while (total < requested) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        // 每次不跨 DMA 块边界，IDF 内部最多等待一次新块，才能遵守总超时。
        // 逐块更新消费计数，避免大 read 误报积压。
        size_t chunk = requested - total;
        const size_t dma_remaining = DMA_FRAMES * FRAME_BYTES - mic->dma_read_offset;
        if (chunk > dma_remaining) {
            chunk = dma_remaining;
        }
        size_t count = 0;
        err = i2s_channel_read(mic->rx, output + total, chunk, &count,
                               (uint32_t)((remaining_us + 999) / 1000));
        total += count;
        mic->dma_read_offset = (mic->dma_read_offset + count) % (DMA_FRAMES * FRAME_BYTES);
        if (consume_bytes(mic, count)) {
            mic->partial_bytes = 0;
            return ESP_ERR_INVALID_SIZE;
        }
        if (err != ESP_OK) {
            break;
        }
        if (!count) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
    }
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        mic->partial_bytes = 0;
        return err;
    }
    *frames_read = total / FRAME_BYTES;
    mic->partial_bytes = total % FRAME_BYTES;
    memcpy(mic->partial, output + *frames_read * FRAME_BYTES, mic->partial_bytes);
    return err;
}

esp_err_t i2s_microphone_stop(i2s_microphone_handle_t mic)
{
    if (!mic) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mic->running) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_disable(mic->rx);
    if (err == ESP_OK) {
        mic->running = false;
        mic->partial_bytes = 0;
    }
    return err;
}

esp_err_t i2s_microphone_destroy(i2s_microphone_handle_t mic)
{
    esp_err_t err = i2s_microphone_stop(mic);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_del_channel(mic->rx);
    if (err == ESP_OK) {
        heap_caps_free(mic);
    }
    return err;
}
