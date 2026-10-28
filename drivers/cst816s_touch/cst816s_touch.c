#include "cst816s_touch.h"

#include <stdatomic.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cst816s_input";
enum { SAMPLE_PERIOD_MS = 10, EVENT_QUEUE_LENGTH = 32 };

struct cst816s_touch {
    cst816s_touch_config_t config;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_handle_t device;
    QueueHandle_t events;
    SemaphoreHandle_t stopped;
    TaskHandle_t sampler;
    bool irq_registered;
    atomic_bool stopping;
};

static TickType_t ticks_at_least_one(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static void touch_irq(void *arg)
{
    cst816s_touch_handle_t touch = arg;
    BaseType_t wake = pdFALSE;
    if (touch->sampler) {
        vTaskNotifyGiveFromISR(touch->sampler, &wake);
        if (wake) {
            portYIELD_FROM_ISR();
        }
    }
}

static esp_err_t create_controller(cst816s_touch_handle_t touch)
{
    // 某些固件仅在复位或触摸后响应，失败后等待下一次 IRQ，不在空闲期反复探测。
    esp_err_t err = i2c_master_probe(touch->config.bus, touch->config.i2c_address, 50);
    if (err != ESP_OK) {
        return err;
    }
    const esp_lcd_touch_config_t config = {
        .x_max = touch->config.native_width,
        .y_max = touch->config.native_height,
        .rst_gpio_num = GPIO_NUM_NC, // 复位可能经过 IO 扩展器，由板级层执行。
        .int_gpio_num = GPIO_NUM_NC, // 中断由采样层独占，官方库不重复注册。
    };
    err = esp_lcd_touch_new_i2c_cst816s(touch->io, &config, &touch->device);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CST816S ready: addr=0x%02x, %lu Hz, raw %ux%u",
                 touch->config.i2c_address, (unsigned long)touch->config.i2c_speed_hz,
                 touch->config.native_width, touch->config.native_height);
    }
    return err;
}

static void touch_sampler(void *arg)
{
    cst816s_touch_handle_t touch = arg;
    cst816s_touch_sample_t last = {0};
    bool suppress_until_release = false;
    bool read_failed = false;
    for (;;) {
        const TickType_t wait = !read_failed && (last.down || suppress_until_release)
                                    ? ticks_at_least_one(SAMPLE_PERIOD_MS) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
        if (atomic_load(&touch->stopping)) {
            break;
        }
        cst816s_touch_sample_t next = last;
        next.cancel = false;
        esp_lcd_touch_point_data_t point = {0};
        uint8_t count = 0;
        esp_err_t err = touch->device ? ESP_OK : create_controller(touch);
        if (err == ESP_OK) {
            err = esp_lcd_touch_read_data(touch->device);
        }
        if (err == ESP_OK) {
            err = esp_lcd_touch_get_data(touch->device, &point, &count, 1);
        }
        if (err != ESP_OK) {
            next.down = false;
            next.ready = false;
            next.cancel = true;
            // 新 IRQ 可以开始新手势，不能因休眠 NACK 锁死下一次首点。
            suppress_until_release = false;
            if (last.ready) {
                ESP_LOGW(TAG, "Touch read failed: %s; gesture cancelled", esp_err_to_name(err));
            }
        } else {
            next.ready = true;
            if (count == 0) {
                next.down = false;
                suppress_until_release = false;
            } else if (point.x >= touch->config.native_width ||
                       point.y >= touch->config.native_height || suppress_until_release) {
                // 超出有效屏区（例如盖板键）后，整次手势保持取消直到松手。
                next.down = false;
                next.cancel = true;
                suppress_until_release = true;
            } else {
                next.down = true;
                next.raw_x = point.x;
                next.raw_y = point.y;
            }
        }
        const bool changed = next.down != last.down || next.ready != last.ready ||
                             (next.down && (next.raw_x != last.raw_x || next.raw_y != last.raw_y));
        if (changed) {
            if (xQueueSend(touch->events, &next, 0) != pdTRUE) {
                // 若消费者来不及读取，清空不完整序列并明确取消，避免遗留按下状态。
                xQueueReset(touch->events);
                next.down = false;
                next.cancel = true;
                xQueueSend(touch->events, &next, 0);
                suppress_until_release = true;
                ESP_LOGW(TAG, "Touch event queue full; gesture cancelled");
            }
            last = next;
        }
        read_failed = err != ESP_OK;
        if (read_failed) {
            vTaskDelay(ticks_at_least_one(20));
        }
    }
    // 此后不再访问 touch，调用者收到信号后才可释放句柄。
    xSemaphoreGive(touch->stopped);
    vTaskDelete(NULL);
}

bool cst816s_touch_read(cst816s_touch_handle_t touch, cst816s_touch_sample_t *sample)
{
    return touch && sample && xQueueReceive(touch->events, sample, 0) == pdTRUE;
}

bool cst816s_touch_has_pending(cst816s_touch_handle_t touch)
{
    return touch && uxQueueMessagesWaiting(touch->events) > 0;
}

esp_err_t cst816s_touch_delete(cst816s_touch_handle_t touch)
{
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_INVALID_ARG, TAG, "NULL handle");
    if (touch->irq_registered) {
        gpio_intr_disable(touch->config.interrupt_gpio);
        gpio_isr_handler_remove(touch->config.interrupt_gpio);
        touch->irq_registered = false;
    }
    if (touch->sampler) {
        // 只在首次请求时通知；超时后任务可能已经自行退出，不能再通知旧任务句柄。
        if (!atomic_exchange(&touch->stopping, true)) {
            xTaskNotifyGive(touch->sampler);
        }
        if (xSemaphoreTake(touch->stopped, ticks_at_least_one(5000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        touch->sampler = NULL;
    }
    if (touch->device) {
        esp_lcd_touch_del(touch->device);
    }
    if (touch->io) {
        esp_lcd_panel_io_del(touch->io);
    }
    if (touch->events) {
        vQueueDelete(touch->events);
    }
    if (touch->stopped) {
        vSemaphoreDelete(touch->stopped);
    }
    free(touch);
    return ESP_OK;
}

esp_err_t cst816s_touch_create(const cst816s_touch_config_t *config,
                              cst816s_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(config && out_touch, ESP_ERR_INVALID_ARG, TAG, "NULL config/output");
    *out_touch = NULL;
    ESP_RETURN_ON_FALSE(config->bus && config->i2c_address > 0 && config->i2c_address < 0x80 &&
                        config->i2c_speed_hz > 0 && GPIO_IS_VALID_GPIO(config->interrupt_gpio) &&
                        config->native_width && config->native_height,
                        ESP_ERR_INVALID_ARG, TAG, "invalid bus, pin or geometry");
    cst816s_touch_handle_t touch = calloc(1, sizeof(*touch));
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_NO_MEM, TAG, "handle allocation failed");
    touch->config = *config;
    atomic_init(&touch->stopping, false);
    const gpio_config_t interrupt_pin = {
        .pin_bit_mask = 1ULL << config->interrupt_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&interrupt_pin);
    if (err != ESP_OK) {
        goto fail;
    }
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_config.dev_addr = config->i2c_address;
    io_config.scl_speed_hz = config->i2c_speed_hz;
    err = esp_lcd_new_panel_io_i2c(config->bus, &io_config, &touch->io);
    if (err != ESP_OK) {
        goto fail;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    touch->events = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(cst816s_touch_sample_t));
    touch->stopped = xSemaphoreCreateBinary();
    if (!touch->events || !touch->stopped) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = gpio_isr_handler_add(config->interrupt_gpio, touch_irq, touch);
    if (err != ESP_OK) {
        goto fail;
    }
    touch->irq_registered = true;
    gpio_intr_disable(config->interrupt_gpio);
    err = gpio_set_intr_type(config->interrupt_gpio, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        goto fail;
    }
    if (xTaskCreate(touch_sampler, "touch_sample", 4096, touch, 3, &touch->sampler) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    // 任务句柄可用后才开放中断；主动通知一次补齐复位到启用中断之间的窗口。
    err = gpio_intr_enable(config->interrupt_gpio);
    if (err != ESP_OK) {
        goto fail;
    }
    xTaskNotifyGive(touch->sampler);
    *out_touch = touch;
    ESP_LOGI(TAG, "Touch input armed on GPIO%d; tap screen to wake if not ready", config->interrupt_gpio);
    return ESP_OK;

fail:
    cst816s_touch_delete(touch);
    return err;
}
