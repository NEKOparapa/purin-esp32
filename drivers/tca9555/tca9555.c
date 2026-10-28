#include "tca9555.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum { REG_INPUT = 0x00, REG_OUTPUT = 0x02, REG_DIRECTION = 0x06 };

struct tca9555_device {
    i2c_master_dev_handle_t io;
    SemaphoreHandle_t mutex;
    int timeout_ms;
};

static esp_err_t read_pair(tca9555_handle_t device, uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(device->io, &reg, 1, data, sizeof(data), device->timeout_ms);
    if (err == ESP_OK) {
        *value = data[0] | ((uint16_t)data[1] << 8);
    }
    return err;
}

static esp_err_t update_pair(tca9555_handle_t device, uint8_t reg, uint16_t mask, uint16_t values)
{
    uint16_t old;
    esp_err_t err = read_pair(device, reg, &old);
    if (err != ESP_OK) {
        return err;
    }
    const uint16_t next = (old & (uint16_t)~mask) | (values & mask);
    if (next == old) {
        return ESP_OK;
    }
    const uint8_t data[] = {reg, (uint8_t)next, (uint8_t)(next >> 8)};
    return i2c_master_transmit(device->io, data, sizeof(data), device->timeout_ms);
}

esp_err_t tca9555_create(i2c_master_bus_handle_t bus, const tca9555_config_t *config,
                         tca9555_handle_t *out_device)
{
    if (!out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_device = NULL;
    if (!bus || !config || config->address < 0x20 || config->address > 0x27 ||
        !config->clock_hz || config->clock_hz > 400000 || config->timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    tca9555_handle_t device = calloc(1, sizeof(*device));
    if (!device) {
        return ESP_ERR_NO_MEM;
    }
    device->mutex = xSemaphoreCreateMutex();
    if (!device->mutex) {
        free(device);
        return ESP_ERR_NO_MEM;
    }
    device->timeout_ms = config->timeout_ms;
    const i2c_device_config_t io_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->address,
        .scl_speed_hz = config->clock_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &io_config, &device->io);
    if (err != ESP_OK) {
        vSemaphoreDelete(device->mutex);
        free(device);
        return err;
    }
    *out_device = device;
    return ESP_OK;
}

esp_err_t tca9555_delete(tca9555_handle_t device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_bus_rm_device(device->io);
    if (err == ESP_OK) {
        vSemaphoreDelete(device->mutex);
        free(device);
    }
    return err;
}

static esp_err_t read_locked(tca9555_handle_t device, uint8_t reg, uint16_t *values)
{
    if (!device || !values) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t err = read_pair(device, reg, values);
    xSemaphoreGive(device->mutex);
    return err;
}

esp_err_t tca9555_read_inputs(tca9555_handle_t device, uint16_t *values)
{
    return read_locked(device, REG_INPUT, values);
}

esp_err_t tca9555_read_outputs(tca9555_handle_t device, uint16_t *values)
{
    return read_locked(device, REG_OUTPUT, values);
}

esp_err_t tca9555_write_outputs(tca9555_handle_t device, uint16_t mask, uint16_t values)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t err = update_pair(device, REG_OUTPUT, mask, values);
    xSemaphoreGive(device->mutex);
    return err;
}

esp_err_t tca9555_configure_outputs(tca9555_handle_t device, uint16_t mask, uint16_t values)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t err = update_pair(device, REG_OUTPUT, mask, values);
    if (err == ESP_OK) {
        err = update_pair(device, REG_DIRECTION, mask, 0);
    }
    xSemaphoreGive(device->mutex);
    return err;
}

esp_err_t tca9555_configure_inputs(tca9555_handle_t device, uint16_t mask)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t err = update_pair(device, REG_DIRECTION, mask, mask);
    xSemaphoreGive(device->mutex);
    return err;
}
