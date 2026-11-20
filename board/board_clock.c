#include "board.h"
#include "board_internal.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static i2c_master_dev_handle_t rtc;

static int decode_bcd(uint8_t byte, int min, int max)
{
    int value = (byte >> 4) * 10 + (byte & 15);
    return (byte & 15) <= 9 && value >= min && value <= max ? value : -1;
}

static uint8_t encode_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void board_clock_init(i2c_master_bus_handle_t bus)
{
    // The original Metalio firmware keeps local civil time in the PCF8563.
    setenv("TZ", "CST-8", 1);
    tzset();
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x51, .scl_speed_hz = 100000,
    };
    if (i2c_master_probe(bus, config.device_address, 100) != ESP_OK ||
        i2c_master_bus_add_device(bus, &config, &rtc) != ESP_OK) return;

    uint8_t first = 0, raw[9];
    if (i2c_master_transmit_receive(rtc, &first, 1, raw, sizeof(raw), 100) != ESP_OK ||
        (raw[0] & 0x20) || (raw[2] & 0x80)) return; // Stopped oscillator or voltage-low flag.
    int day = decode_bcd(raw[5] & 63, 1, 31), month = decode_bcd(raw[7] & 31, 1, 12);
    int year = decode_bcd(raw[8], 20, 99);
    struct tm date = {
        .tm_sec = decode_bcd(raw[2] & 127, 0, 59), .tm_min = decode_bcd(raw[3] & 127, 0, 59),
        .tm_hour = decode_bcd(raw[4] & 63, 0, 23), .tm_mday = day, .tm_mon = month - 1,
        .tm_year = 100 + year, .tm_isdst = -1,
    };
    if (date.tm_sec < 0 || date.tm_min < 0 || date.tm_hour < 0 || day < 0 || month < 0 || year < 0) return;
    time_t epoch = mktime(&date);
    if (epoch < 0 || date.tm_mday != day || date.tm_mon != month - 1) return;
    struct timeval tv = {.tv_sec = epoch};
    settimeofday(&tv, NULL);
}

esp_err_t board_clock_store(void)
{
    time_t now = time(NULL);
    struct tm date;
    if (!rtc || !localtime_r(&now, &date) || date.tm_year < 120 || date.tm_year > 199)
        return ESP_ERR_INVALID_STATE;
    uint8_t bytes[] = {2, encode_bcd(date.tm_sec), encode_bcd(date.tm_min), encode_bcd(date.tm_hour),
                       encode_bcd(date.tm_mday), (uint8_t)date.tm_wday, encode_bcd(date.tm_mon + 1),
                       encode_bcd(date.tm_year - 100)};
    esp_err_t err = i2c_master_transmit(rtc, bytes, sizeof(bytes), 100);
    // Start an oscillator that was previously stopped, after loading the complete date.
    const uint8_t start[] = {0, 0};
    return err == ESP_OK ? i2c_master_transmit(rtc, start, sizeof(start), 100) : err;
}
