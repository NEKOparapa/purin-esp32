#include "board.h"
#include "board_config.h"
#include "board_internal.h"

#include <stdatomic.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Metalio E-Ink 4 voltage-based display policy:
// https://github.com/CloudZao/Metalio-E-INK4/blob/d12f481f4a42420634bb56a928f3959a977367d4/docs/battery-soc-and-charge.md
enum {
    REG_VOLTAGE_MV = 0x08, REG_CURRENT_MA = 0x0c,
    EMPTY_MV = 3300, FULL_MV = 4330, FULL_RELEASE_MV = 4280,
    CURRENT_DEADBAND_MA = 5, FILTER_SAMPLES = 60, I2C_TIMEOUT_MS = 50,
};

static atomic_int battery_percent = -1;

int board_battery_percent(void)
{
    return atomic_load(&battery_percent);
}

typedef struct {
    uint16_t millivolts[FILTER_SAMPLES];
    uint32_t sum_mv;
    unsigned next;
    int displayed, direction;
    bool above_full;
    TickType_t full_since, last_step;
} battery_filter_t;

static int update_percent(battery_filter_t *f, uint16_t mv, int16_t ma, TickType_t now)
{
    // Clamp each sample before averaging, and convert the mean to percent just once.
    uint16_t sample = mv < EMPTY_MV ? EMPTY_MV : mv > FULL_MV ? FULL_MV : mv;
    if (f->displayed < 0) {
        for (unsigned i = 0; i < FILTER_SAMPLES; ++i) f->millivolts[i] = sample;
        f->sum_mv = sample * FILTER_SAMPLES;
        f->above_full = false;
        f->last_step = now;
    }
    f->sum_mv = f->sum_mv - f->millivolts[f->next] + sample;
    f->millivolts[f->next] = sample;
    f->next = (f->next + 1) % FILTER_SAMPLES;
    const int range = FILTER_SAMPLES * (FULL_MV - EMPTY_MV);
    int target = ((f->sum_mv - FILTER_SAMPLES * EMPTY_MV) * 100 + range / 2) / range;

    const TickType_t hold = pdMS_TO_TICKS(30000);
    if (mv >= FULL_MV && !f->above_full) f->full_since = now;
    f->above_full = mv >= FULL_MV;
    const bool full = f->above_full && (now - f->full_since >= hold);
    if (target == 100 && !full) target = 99;
    if (f->displayed == 100 && mv >= FULL_RELEASE_MV) target = 100;

    int direction = (ma > CURRENT_DEADBAND_MA) - (ma < -CURRENT_DEADBAND_MA);
    if (direction != f->direction) f->last_step = now;
    f->direction = direction;
    if (f->displayed < 0 || (full && target == 100 && f->displayed >= 99)) {
        f->displayed = target;
    } else if (now - f->last_step >= hold) {
        if (direction > 0 && target > f->displayed) {
            ++f->displayed;
            f->last_step = now;
        } else if (direction <= 0 && target < f->displayed) {
            --f->displayed;
            f->last_step = now;
        }
    }
    return f->displayed;
}

static bool read_word(i2c_master_dev_handle_t gauge, uint8_t reg, uint16_t *word)
{
    uint8_t data[2];
    if (i2c_master_transmit_receive(gauge, &reg, 1, data, sizeof(data), I2C_TIMEOUT_MS) != ESP_OK)
        return false;
    *word = data[0] | ((uint16_t)data[1] << 8);
    return true;
}

static void battery_task(void *arg)
{
    i2c_master_bus_handle_t bus = arg;
    i2c_master_dev_handle_t gauge = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_GAUGE_I2C_ADDRESS,
        .scl_speed_hz = BOARD_GAUGE_I2C_HZ,
    };
    battery_filter_t filter = { .displayed = -1 };
    for (;;) {
        if (!gauge && i2c_master_probe(bus, cfg.device_address, I2C_TIMEOUT_MS) == ESP_OK) {
            i2c_master_dev_handle_t device;
            if (i2c_master_bus_add_device(bus, &cfg, &device) == ESP_OK) gauge = device;
        }
        int percent = -1;
        uint16_t mv, raw_ma;
        if (gauge && read_word(gauge, REG_VOLTAGE_MV, &mv) && mv > 0 && mv <= 5000 &&
            read_word(gauge, REG_CURRENT_MA, &raw_ma)) {
            percent = update_percent(&filter, mv, (int16_t)raw_ma, xTaskGetTickCount());
        } else {
            // Publish unknown now; anchor the filter to the next valid reading.
            filter.displayed = -1;
        }
        atomic_store(&battery_percent, percent);
        vTaskDelay(pdMS_TO_TICKS(percent < 0 ? 10000 : 1000));
    }
}

void board_battery_init(i2c_master_bus_handle_t bus)
{
    if (xTaskCreate(battery_task, "battery", 3072, bus, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGW("board", "Battery monitor unavailable");
    }
}
