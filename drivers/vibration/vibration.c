#include "vibration.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "vibration";

struct vibration_device {
    vibration_config_t config;
    SemaphoreHandle_t mutex;
    esp_timer_handle_t timer;
    int64_t deadline_us;
    bool active;
};

// 以下两个辅助函数由持有 mutex 的调用方使用。
static esp_err_t turn_off(vibration_handle_t motor)
{
    esp_err_t err = gpio_set_level(motor->config.gpio, motor->config.active_high ? 0 : 1);
    if (err == ESP_OK) {
        motor->active = false;
    }
    return err;
}

static esp_err_t cancel_timer(vibration_handle_t motor)
{
    esp_err_t err = esp_timer_stop(motor->timer);
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err; // 已到期/未启动均可取消。
}

static void on_timeout(void *arg)
{
    vibration_handle_t motor = arg;
    xSemaphoreTake(motor->mutex, portMAX_DELAY);
    // stop 不保证取消已经分发的回调。旧回调必须检查新截止时间，
    // 否则连续点击可能被上一次脉冲的过期事件提前关闭。
    if (motor->active && esp_timer_get_time() >= motor->deadline_us) {
        esp_err_t err = turn_off(motor);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Motor GPIO%d off failed: %s", motor->config.gpio, esp_err_to_name(err));
        }
    }
    xSemaphoreGive(motor->mutex);
}

esp_err_t vibration_create(const vibration_config_t *config, vibration_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!config || !GPIO_IS_VALID_OUTPUT_GPIO(config->gpio) || !config->default_pulse_ms ||
        config->max_pulse_ms < config->default_pulse_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    vibration_handle_t motor = calloc(1, sizeof(*motor));
    if (!motor) {
        return ESP_ERR_NO_MEM;
    }
    motor->config = *config;
    motor->mutex = xSemaphoreCreateMutex();
    if (!motor->mutex) {
        free(motor);
        return ESP_ERR_NO_MEM;
    }
    // 先写关闭电平锁存，再使能输出，防止初始化时出现短暂误震。
    esp_err_t err = turn_off(motor);
    if (err != ESP_OK) {
        goto fail;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << config->gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) {
        goto fail;
    }
    err = gpio_sleep_sel_dis(config->gpio);
    if (err != ESP_OK) {
        goto fail;
    }
    const esp_timer_create_args_t timer = {
        .callback = on_timeout,
        .arg = motor,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vibration_off",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer, &motor->timer);
    if (err != ESP_OK) {
        goto fail;
    }
    *out_handle = motor;
    return ESP_OK;

fail:
    // 定时器尚未启动，无在途回调；失败对象可以立即释放。
    (void)turn_off(motor);
    vSemaphoreDelete(motor->mutex);
    free(motor);
    return err;
}

esp_err_t vibration_pulse(vibration_handle_t motor, uint32_t duration_ms)
{
    if (!motor) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t ms = duration_ms ? duration_ms : motor->config.default_pulse_ms;
    if (ms > motor->config.max_pulse_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(motor->mutex, portMAX_DELAY);
    esp_err_t err = cancel_timer(motor);
    if (err == ESP_OK) {
        motor->deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
        // 先确保关断定时器已启动，再开启输出，避免启动计时失败后马达常开。
        err = esp_timer_start_once(motor->timer, (uint64_t)ms * 1000);
    }
    if (err == ESP_OK) {
        err = gpio_set_level(motor->config.gpio, motor->config.active_high ? 1 : 0);
        if (err == ESP_OK) {
            motor->active = true;
        }
    }
    if (err != ESP_OK) {
        (void)cancel_timer(motor);
        motor->deadline_us = 0;
        esp_err_t off_err = turn_off(motor);
        if (off_err != ESP_OK) {
            ESP_LOGE(TAG, "Pulse cleanup GPIO%d failed: %s", motor->config.gpio, esp_err_to_name(off_err));
        }
    }
    xSemaphoreGive(motor->mutex);
    return err;
}

esp_err_t vibration_stop(vibration_handle_t motor)
{
    if (!motor) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(motor->mutex, portMAX_DELAY);
    esp_err_t timer_err = cancel_timer(motor);
    motor->deadline_us = 0;
    esp_err_t gpio_err = turn_off(motor);
    xSemaphoreGive(motor->mutex);
    return gpio_err != ESP_OK ? gpio_err : timer_err;
}

bool vibration_is_active(vibration_handle_t motor)
{
    if (!motor) {
        return false;
    }
    xSemaphoreTake(motor->mutex, portMAX_DELAY);
    bool active = motor->active;
    xSemaphoreGive(motor->mutex);
    return active;
}
