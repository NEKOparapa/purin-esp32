#include "board.h"

#include "board_config.h"
#include "vibration.h"

static vibration_handle_t motor;

esp_err_t board_vibration_init(void)
{
    if (motor) {
        return ESP_OK;
    }
    const vibration_config_t config = {
        .gpio = BOARD_VIBRATION_GPIO,
        .active_high = BOARD_VIBRATION_ACTIVE_HIGH,
        .default_pulse_ms = BOARD_VIBRATION_PULSE_MS,
        .max_pulse_ms = BOARD_VIBRATION_MAX_MS,
    };
    return vibration_create(&config, &motor);
}

esp_err_t board_vibration_pulse(uint32_t duration_ms)
{
    return motor ? vibration_pulse(motor, duration_ms) : ESP_ERR_INVALID_STATE;
}

esp_err_t board_vibration_stop(void)
{
    return motor ? vibration_stop(motor) : ESP_ERR_INVALID_STATE;
}

bool board_vibration_is_active(void)
{
    return motor && vibration_is_active(motor);
}
