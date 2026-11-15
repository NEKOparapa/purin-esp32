#include "terminal_network.h"
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

enum { WIFI_CONNECTED = BIT0, WIFI_ENABLED = BIT1 };
static EventGroupHandle_t wifi_events;
static bool wifi_ready;
static SemaphoreHandle_t network_lock;

bool terminal_configured(void)
{
    return WINTERMUTE_WIFI_SSID[0] && WINTERMUTE_SERVER_URL[0] && WINTERMUTE_TERMINAL_ID[0] &&
           WINTERMUTE_TERMINAL_TOKEN[0] &&
           (!strncmp(WINTERMUTE_SERVER_URL, "https://", 8) || !strncmp(WINTERMUTE_SERVER_URL, "http://", 7));
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED);
    } else if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
        if (xEventGroupGetBits(wifi_events) & WIFI_ENABLED) {
            (void)esp_wifi_connect();
        }
    }
}

static esp_err_t wifi_init(void)
{
    if (wifi_ready) {
        return ESP_OK;
    }
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        return error; // 不擦除已有 NVS。
    }
    error = esp_netif_init();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    wifi_events = xEventGroupCreate();
    if (!wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if (error != ESP_OK) {
        esp_netif_destroy_default_wifi(netif);
        vEventGroupDelete(wifi_events);
        wifi_events = NULL;
        return error;
    }
    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (error == ESP_OK) {
        error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    }
    wifi_config_t config = {0};
    if (strlen(WINTERMUTE_WIFI_SSID) > sizeof(config.sta.ssid) ||
        strlen(WINTERMUTE_WIFI_PASSWORD) > sizeof(config.sta.password)) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) {
        memcpy(config.sta.ssid, WINTERMUTE_WIFI_SSID, strlen(WINTERMUTE_WIFI_SSID));
        memcpy(config.sta.password, WINTERMUTE_WIFI_PASSWORD, strlen(WINTERMUTE_WIFI_PASSWORD));
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (error != ESP_OK) {
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event);
        (void)esp_wifi_deinit();
        esp_netif_destroy_default_wifi(netif);
        vEventGroupDelete(wifi_events);
        wifi_events = NULL;
        return error;
    }
    wifi_ready = true;
    return ESP_OK;
}

static esp_err_t connect_wifi(void)
{
    esp_err_t error = wifi_init();
    if (error != ESP_OK) {
        return error;
    }
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
    xEventGroupSetBits(wifi_events, WIFI_ENABLED);
    error = esp_wifi_start();
    if (error != ESP_OK) {
        return error;
    }
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    return bits & WIFI_CONNECTED ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t terminal_network_init(void)
{
    if (!network_lock) {
        network_lock = xSemaphoreCreateMutex();
    }
    return network_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void terminal_network_release(void)
{
    if (wifi_ready) {
        xEventGroupClearBits(wifi_events, WIFI_ENABLED | WIFI_CONNECTED);
        (void)esp_wifi_stop();
    }
    xSemaphoreGive(network_lock);
}

esp_err_t terminal_network_acquire(void)
{
    if (!network_lock || !terminal_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(network_lock, portMAX_DELAY);
    esp_err_t error = connect_wifi();
    if (error != ESP_OK) {
        terminal_network_release();
    }
    return error;
}
