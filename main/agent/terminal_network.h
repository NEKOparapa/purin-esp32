#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef WINTERMUTE_LOCAL_UPLOAD_CONFIG
#include "upload_config.local.h"
#else
#include "upload_config.example.h"
#endif
#if !defined(WINTERMUTE_DEVICE_TOKEN) && defined(WINTERMUTE_TERMINAL_TOKEN)
#define WINTERMUTE_DEVICE_TOKEN WINTERMUTE_TERMINAL_TOKEN
#endif
#ifndef WINTERMUTE_DEVICE_ID
#ifdef WINTERMUTE_TERMINAL_ID
#define WINTERMUTE_DEVICE_ID WINTERMUTE_TERMINAL_ID
#else
#define WINTERMUTE_DEVICE_ID ""
#endif
#endif
#ifndef WINTERMUTE_DEVICE_NAME
#define WINTERMUTE_DEVICE_NAME "ESP32"
#endif
#define WINTERMUTE_DEVICE_PLATFORM "esp32"

const char *wintermute_device_id(void);
const char *wintermute_device_instance_id(void);

typedef enum {
    TERMINAL_NETWORK_WIFI,
    TERMINAL_NETWORK_CELLULAR,
    TERMINAL_NETWORK_OFF,
} terminal_network_mode_t;

typedef enum {
    TERMINAL_LINK_IDLE,
    TERMINAL_LINK_CONNECTING,
    TERMINAL_LINK_CONNECTED,
    TERMINAL_LINK_NO_SSID,
    TERMINAL_LINK_NO_SIM,
    TERMINAL_LINK_FAILED,
} terminal_link_state_t;

enum { TERMINAL_WIFI_SSID_SIZE = 33, TERMINAL_WIFI_PASSWORD_SIZE = 65 };

typedef struct {
    char ssid[TERMINAL_WIFI_SSID_SIZE];
    int8_t rssi;
    bool secured;
} terminal_wifi_ap_t;

typedef enum {
    TERMINAL_WIFI_JOIN_OK,
    TERMINAL_WIFI_JOIN_INVALID_PASSWORD,
    TERMINAL_WIFI_JOIN_AUTH_FAILED,
    TERMINAL_WIFI_JOIN_FAILED,
    TERMINAL_WIFI_JOIN_SAVE_FAILED,
} terminal_wifi_join_result_t;

bool terminal_configured(void);
esp_err_t terminal_network_init(void);
terminal_network_mode_t terminal_network_mode(void);
terminal_link_state_t terminal_network_link_state(void);
// Saves the mode and connects the selected radio, even without backend credentials.
// Runs on a worker and waits for any current network transaction to finish.
esp_err_t terminal_network_select(terminal_network_mode_t mode);
bool terminal_wifi_saved_ssid(char ssid[TERMINAL_WIFI_SSID_SIZE]);
// Scan and join run on a worker while Wi-Fi is selected.
esp_err_t terminal_wifi_scan(terminal_wifi_ap_t *aps, size_t capacity, size_t *count);
terminal_wifi_join_result_t terminal_wifi_join(const terminal_wifi_ap_t *ap, const char *password);
// Each successful acquire must be paired with a release, on the same worker.
esp_err_t terminal_network_acquire(void);
void terminal_network_release(void);
