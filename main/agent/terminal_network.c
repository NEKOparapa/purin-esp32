#include "terminal_network.h"
#include "terminal_http.h"
#include "terminal_pairing.h"
#include <stdatomic.h>
#include <stdio.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "board.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

enum {
    WIFI_CONNECTED = BIT0,
    WIFI_JOIN = BIT1,
    WIFI_STA_STARTED = BIT2,
    WIFI_TRIAL = BIT3,
    WIFI_AUTH_FAILED = BIT4,
    WIFI_SCANNING = BIT5,
    WIFI_BITS = WIFI_CONNECTED | WIFI_JOIN | WIFI_STA_STARTED | WIFI_TRIAL |
                WIFI_AUTH_FAILED | WIFI_SCANNING,
};

typedef struct {
    char ssid[TERMINAL_WIFI_SSID_SIZE];
    char password[TERMINAL_WIFI_PASSWORD_SIZE];
} wifi_profile_t;

static EventGroupHandle_t wifi_events;
static bool wifi_ready;
static bool wifi_started;
static bool netif_ready;
static bool profile_sent;
static SemaphoreHandle_t network_lock;
static char factory_id[19];
static atomic_int selected_mode = TERMINAL_NETWORK_WIFI;
static atomic_int link_state = TERMINAL_LINK_IDLE;
static int64_t cellular_retry_at;
static wifi_profile_t wifi_profile;
static portMUX_TYPE wifi_profile_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const PREF_NAMESPACE = "pr_network";
static const char *const PREF_MODE = "mode";
static const char *const PREF_WIFI = "wifi";

static void reconnect_wifi_on_boot(void *unused);

const char *purin_device_instance_id(void) { return factory_id; }

bool terminal_configured(void)
{
    terminal_network_mode_t mode = terminal_network_mode();
    return mode != TERMINAL_NETWORK_OFF && (mode != TERMINAL_NETWORK_WIFI || terminal_wifi_saved_ssid(NULL)) &&
           terminal_device_token()[0] && factory_id[0] &&
           (!strncmp(terminal_server_url(), "https://", 8) || !strncmp(terminal_server_url(), "http://", 7));
}

terminal_network_mode_t terminal_network_mode(void)
{
    return atomic_load(&selected_mode);
}

terminal_link_state_t terminal_network_link_state(void)
{
    terminal_link_state_t state = atomic_load(&link_state);
    if (terminal_network_mode() == TERMINAL_NETWORK_WIFI && wifi_events &&
        (xEventGroupGetBits(wifi_events) & (WIFI_CONNECTED | WIFI_TRIAL)) == WIFI_CONNECTED) {
        return TERMINAL_LINK_CONNECTED;
    }
    if (state != TERMINAL_LINK_CONNECTED) return state;
    switch (terminal_network_mode()) {
    case TERMINAL_NETWORK_WIFI:
        return wifi_events && (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED)
            ? TERMINAL_LINK_CONNECTED : TERMINAL_LINK_CONNECTING;
    case TERMINAL_NETWORK_CELLULAR:
        return board_cellular_connected() ? TERMINAL_LINK_CONNECTED : TERMINAL_LINK_IDLE;
    default:
        return TERMINAL_LINK_IDLE;
    }
}

bool terminal_wifi_saved_ssid(char ssid[TERMINAL_WIFI_SSID_SIZE])
{
    portENTER_CRITICAL(&wifi_profile_lock);
    bool configured = wifi_profile.ssid[0] != '\0';
    if (ssid) memcpy(ssid, wifi_profile.ssid, sizeof(wifi_profile.ssid));
    portEXIT_CRITICAL(&wifi_profile_lock);
    return configured;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    EventBits_t bits = xEventGroupGetBits(wifi_events);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        bits = xEventGroupSetBits(wifi_events, WIFI_STA_STARTED);
        if (bits & WIFI_JOIN) (void)esp_wifi_connect();
        return;
    }
    // A disconnect/IP event queued before a stop must not finish the next join.
    if ((bits & (WIFI_JOIN | WIFI_STA_STARTED)) != (WIFI_JOIN | WIFI_STA_STARTED)) return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
        const wifi_event_sta_disconnected_t *event = data;
        if ((bits & WIFI_TRIAL) && event &&
            (event->reason == WIFI_REASON_AUTH_FAIL || event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
             event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT)) {
            xEventGroupSetBits(wifi_events, WIFI_AUTH_FAILED);
            return;
        }
        if (!(bits & WIFI_SCANNING)) (void)esp_wifi_connect();
    }
}

static esp_err_t network_stack_init(void)
{
    if (netif_ready) return ESP_OK;
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        ESP_LOGW("terminal", "Default event loop: %s (internal free=%u, largest=%u)",
                 esp_err_to_name(error), (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_largest_free_block(caps));
        // The event library returns ESP_FAIL when the loop task cannot be allocated.
        return error == ESP_FAIL ? ESP_ERR_NO_MEM : error;
    }
    netif_ready = true;
    return ESP_OK;
}

static esp_err_t wifi_init(void)
{
    if (wifi_ready) return ESP_OK;
    const esp_netif_config_t config = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t *netif = esp_netif_new(&config);
    if (!netif) return ESP_ERR_NO_MEM;
    esp_err_t error = esp_netif_attach_wifi_station(netif);
    if (error == ESP_OK) error = esp_wifi_set_default_wifi_sta_handlers();
    if (error == ESP_OK) {
        const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&init);
    }
    if (error != ESP_OK) {
        esp_netif_destroy_default_wifi(netif);
        return error;
    }
    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (error == ESP_OK) {
        error = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error != ESP_OK) {
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        (void)esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event);
        esp_netif_destroy_default_wifi(netif);
        (void)esp_wifi_deinit();
        return error;
    }
    wifi_ready = true;
    return ESP_OK;
}

static void stop_wifi(void)
{
    if (!wifi_started) return;
    xEventGroupClearBits(wifi_events, WIFI_BITS);
    (void)esp_wifi_stop();
    wifi_started = false;
}

static esp_err_t start_wifi(const wifi_profile_t *profile, bool trial)
{
    if (wifi_started) return ESP_OK;
    esp_err_t error = network_stack_init();
    if (error == ESP_OK) error = wifi_init();
    if (error != ESP_OK) return error;

    xEventGroupClearBits(wifi_events, WIFI_BITS);
    if (profile) {
        wifi_config_t config = {0};
        size_t password_len = strlen(profile->password);
        memcpy(config.sta.ssid, profile->ssid, strlen(profile->ssid));
        memcpy(config.sta.password, profile->password, password_len);
        config.sta.threshold.authmode = password_len >= 8 ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
        mbedtls_platform_zeroize(&config, sizeof(config));
        if (error != ESP_OK) return error;
        xEventGroupSetBits(wifi_events, WIFI_JOIN | (trial ? WIFI_TRIAL : 0));
    }
    error = esp_wifi_start();
    if (error == ESP_OK) wifi_started = true;
    else xEventGroupClearBits(wifi_events, WIFI_BITS);
    return error;
}

static EventBits_t wait_wifi(bool trial)
{
    return xEventGroupWaitBits(wifi_events, WIFI_CONNECTED | (trial ? WIFI_AUTH_FAILED : 0),
                               pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
}

static esp_err_t connect_wifi(void)
{
    esp_err_t error = start_wifi(&wifi_profile, false);
    if (error != ESP_OK) return error;
    return (wait_wifi(false) & WIFI_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t terminal_network_init(void)
{
    if (network_lock) return ESP_OK;
    uint8_t mac[6];
    esp_err_t error = esp_efuse_mac_get_default(mac);
    if (error != ESP_OK) return error;
    snprintf(factory_id, sizeof(factory_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_err_t store_error = nvs_flash_init();
    if (store_error == ESP_OK) terminal_load_pairing();
    ESP_LOGI("terminal", "Device: %s, device ID: %s", PURIN_DEVICE_NAME, purin_device_id());
    nvs_handle_t prefs;
    if (store_error == ESP_OK) store_error = nvs_open(PREF_NAMESPACE, NVS_READONLY, &prefs);
    if (store_error == ESP_OK) {
        uint8_t value;
        store_error = nvs_get_u8(prefs, PREF_MODE, &value);
        if (store_error == ESP_OK && value <= TERMINAL_NETWORK_OFF) atomic_store(&selected_mode, value);
        size_t bytes = sizeof(wifi_profile);
        if (nvs_get_blob(prefs, PREF_WIFI, &wifi_profile, &bytes) != ESP_OK || bytes != sizeof(wifi_profile) ||
            wifi_profile.ssid[sizeof(wifi_profile.ssid) - 1] ||
            wifi_profile.password[sizeof(wifi_profile.password) - 1]) {
            mbedtls_platform_zeroize(&wifi_profile, sizeof(wifi_profile));
        }
        nvs_close(prefs);
    }
    if (store_error != ESP_OK && store_error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW("terminal", "Load network preference: %s", esp_err_to_name(store_error));
    }
    network_lock = xSemaphoreCreateMutex();
    wifi_events = xEventGroupCreate();
    if (!network_lock || !wifi_events) {
        if (network_lock) vSemaphoreDelete(network_lock);
        if (wifi_events) vEventGroupDelete(wifi_events);
        network_lock = NULL;
        wifi_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    error = network_stack_init();
    if (error == ESP_OK && terminal_network_mode() == TERMINAL_NETWORK_WIFI) {
        error = wifi_init();
    }
    if (error == ESP_OK && terminal_network_mode() == TERMINAL_NETWORK_WIFI && wifi_profile.ssid[0] &&
        xTaskCreate(reconnect_wifi_on_boot, "wifi_boot", 6144, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW("terminal", "Cannot start Wi-Fi reconnect task");
    }
    return error;
}

void terminal_network_release(void)
{
    xSemaphoreGive(network_lock);
}

// Called under the existing network lock after the selected interface has an IP address.
static void sync_calendar_clock(void)
{
    static int64_t next_attempt;
    int64_t now = esp_timer_get_time();
    if (now < next_attempt) return;
    next_attempt = now + 600LL * 1000000;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&config) != ESP_OK) return;
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(6500)) == ESP_OK) {
        board_clock_store();
        next_attempt = now + 86400LL * 1000000;
    }
    esp_netif_sntp_deinit();
}

static esp_err_t connect_selected(void)
{
    terminal_network_mode_t mode = terminal_network_mode();
    if (mode == TERMINAL_NETWORK_OFF) return ESP_ERR_INVALID_STATE;
    if (mode == TERMINAL_NETWORK_WIFI && !wifi_profile.ssid[0]) {
        atomic_store(&link_state, TERMINAL_LINK_NO_SSID);
        return ESP_ERR_NOT_FOUND;
    }
    if (mode == TERMINAL_NETWORK_CELLULAR && esp_timer_get_time() < cellular_retry_at) {
        return ESP_ERR_TIMEOUT;
    }
    if (terminal_network_link_state() == TERMINAL_LINK_CONNECTED) return ESP_OK;

    atomic_store(&link_state, TERMINAL_LINK_CONNECTING);
    esp_err_t error = network_stack_init();
    if (error == ESP_OK) {
        error = mode == TERMINAL_NETWORK_WIFI ? connect_wifi() : board_cellular_connect();
    }
    terminal_link_state_t state = TERMINAL_LINK_CONNECTED;
    if (error != ESP_OK) {
        state = mode == TERMINAL_NETWORK_CELLULAR && error == ESP_ERR_NOT_FOUND
            ? TERMINAL_LINK_NO_SIM : TERMINAL_LINK_FAILED;
        if (mode == TERMINAL_NETWORK_CELLULAR) {
            cellular_retry_at = esp_timer_get_time() + 30000000LL;
        }
    }
    atomic_store(&link_state, state);
    return error;
}

static void reconnect_wifi_on_boot(void *unused)
{
    (void)unused;
    xSemaphoreTake(network_lock, portMAX_DELAY);
    if (terminal_network_mode() == TERMINAL_NETWORK_WIFI && connect_selected() == ESP_OK) {
        sync_calendar_clock();
    }
    xSemaphoreGive(network_lock);
    vTaskDelete(NULL);
}

static bool scan_auth_supported(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return true;
    default:
        return false;
    }
}

esp_err_t terminal_wifi_scan(terminal_wifi_ap_t *aps, size_t capacity, size_t *count)
{
    if (!aps || !capacity || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    if (!network_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(network_lock, portMAX_DELAY);
    if (terminal_network_mode() != TERMINAL_NETWORK_WIFI) {
        xSemaphoreGive(network_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const bool already_running = wifi_started;
    esp_err_t error = start_wifi(NULL, false);
    if (error == ESP_OK) {
        xEventGroupSetBits(wifi_events, WIFI_SCANNING);
        if (already_running && !(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED)) {
            (void)esp_wifi_disconnect();
        }
        error = esp_wifi_scan_start(NULL, true);
        if (error == ESP_OK) {
            wifi_ap_record_t record;
            while (*count < capacity && esp_wifi_scan_get_ap_record(&record) == ESP_OK) {
                if (!record.ssid[0] || !scan_auth_supported(record.authmode)) continue;
                bool secured = record.authmode != WIFI_AUTH_OPEN;
                size_t i = 0;
                while (i < *count && (aps[i].secured != secured ||
                       strncmp(aps[i].ssid, (const char *)record.ssid, TERMINAL_WIFI_SSID_SIZE - 1))) ++i;
                if (i < *count) continue; // The ESP-IDF returns strongest APs first.
                strlcpy(aps[i].ssid, (const char *)record.ssid, sizeof(aps[i].ssid));
                aps[i].rssi = record.rssi;
                aps[i].secured = secured;
                ++*count;
            }
        }
        (void)esp_wifi_clear_ap_list();
        xEventGroupClearBits(wifi_events, WIFI_SCANNING);
        if (!already_running) stop_wifi();
        else if (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED)) (void)esp_wifi_connect();
    }
    xSemaphoreGive(network_lock);
    if (error != ESP_OK) {
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        ESP_LOGW("terminal", "Wi-Fi scan failed: %s (internal free=%u, largest=%u)",
                 esp_err_to_name(error), (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_largest_free_block(caps));
    }
    return error;
}

static bool valid_wifi_password(bool secured, const char *password)
{
    size_t length = strlen(password);
    if (!secured) return length == 0;
    if (length < 8 || length >= TERMINAL_WIFI_PASSWORD_SIZE) return false;
    if (length == TERMINAL_WIFI_PASSWORD_SIZE - 1) {
        return strspn(password, "0123456789abcdefABCDEF") == length;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = password[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static esp_err_t save_wifi_profile(const wifi_profile_t *profile)
{
    nvs_handle_t prefs;
    esp_err_t error = nvs_open(PREF_NAMESPACE, NVS_READWRITE, &prefs);
    if (error == ESP_OK) {
        error = nvs_set_blob(prefs, PREF_WIFI, profile, sizeof(*profile));
        if (error == ESP_OK) error = nvs_commit(prefs);
        nvs_close(prefs);
    }
    return error;
}

terminal_wifi_join_result_t terminal_wifi_join(const terminal_wifi_ap_t *ap, const char *password)
{
    if (!ap || !ap->ssid[0] || !memchr(ap->ssid, '\0', sizeof(ap->ssid)) || !network_lock) {
        return TERMINAL_WIFI_JOIN_FAILED;
    }
    if (!password || !valid_wifi_password(ap->secured, password)) return TERMINAL_WIFI_JOIN_INVALID_PASSWORD;
    xSemaphoreTake(network_lock, portMAX_DELAY);
    if (terminal_network_mode() != TERMINAL_NETWORK_WIFI) {
        xSemaphoreGive(network_lock);
        return TERMINAL_WIFI_JOIN_FAILED;
    }

    wifi_profile_t candidate = {0};
    strlcpy(candidate.ssid, ap->ssid, sizeof(candidate.ssid));
    strlcpy(candidate.password, password, sizeof(candidate.password));
    terminal_wifi_join_result_t result = TERMINAL_WIFI_JOIN_FAILED;
    stop_wifi();
    atomic_store(&link_state, TERMINAL_LINK_CONNECTING);
    if (start_wifi(&candidate, true) == ESP_OK) {
        EventBits_t bits = wait_wifi(true);
        if (bits & WIFI_CONNECTED) {
            result = save_wifi_profile(&candidate) == ESP_OK
                ? TERMINAL_WIFI_JOIN_OK : TERMINAL_WIFI_JOIN_SAVE_FAILED;
        } else if (bits & WIFI_AUTH_FAILED) {
            result = TERMINAL_WIFI_JOIN_AUTH_FAILED;
        }
    }
    if (result == TERMINAL_WIFI_JOIN_OK) {
        portENTER_CRITICAL(&wifi_profile_lock);
        wifi_profile = candidate;
        portEXIT_CRITICAL(&wifi_profile_lock);
        xEventGroupClearBits(wifi_events, WIFI_TRIAL | WIFI_AUTH_FAILED);
        atomic_store(&link_state, TERMINAL_LINK_CONNECTED);
        // The test connection can drop before automatic reconnect is re-enabled.
        if (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED)) (void)esp_wifi_connect();
        sync_calendar_clock();
    } else {
        stop_wifi();
        // The old profile stays stored and is reconnected if the test fails.
        if (wifi_profile.ssid[0] && start_wifi(&wifi_profile, false) == ESP_OK) {
            atomic_store(&link_state, TERMINAL_LINK_CONNECTING);
        } else {
            atomic_store(&link_state, wifi_profile.ssid[0] ? TERMINAL_LINK_FAILED : TERMINAL_LINK_NO_SSID);
        }
    }
    mbedtls_platform_zeroize(&candidate, sizeof(candidate));
    xSemaphoreGive(network_lock);
    return result;
}

esp_err_t terminal_network_select(terminal_network_mode_t mode)
{
    if ((unsigned)mode > TERMINAL_NETWORK_OFF) return ESP_ERR_INVALID_ARG;
    if (!network_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(network_lock, portMAX_DELAY);
    terminal_network_mode_t previous = terminal_network_mode();
    esp_err_t error = ESP_OK;
    if (mode != previous) {
        nvs_handle_t prefs;
        error = nvs_open(PREF_NAMESPACE, NVS_READWRITE, &prefs);
        if (error == ESP_OK) {
            error = nvs_set_u8(prefs, PREF_MODE, mode);
            if (error == ESP_OK) error = nvs_commit(prefs);
            nvs_close(prefs);
        }
        if (error == ESP_OK) {
            atomic_store(&link_state, TERMINAL_LINK_IDLE);
            if (previous == TERMINAL_NETWORK_WIFI) stop_wifi();
            if (previous == TERMINAL_NETWORK_CELLULAR) board_cellular_stop();
            atomic_store(&selected_mode, mode);
        }
    }
    if (error == ESP_OK && mode != TERMINAL_NETWORK_OFF) {
        cellular_retry_at = 0; // An explicit tap also retries a failed connection.
        error = connect_selected();
        if (error == ESP_OK) sync_calendar_clock();
    }
    xSemaphoreGive(network_lock);
    return error;
}

esp_err_t terminal_network_acquire_link(void)
{
    if (!network_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(network_lock, portMAX_DELAY);
    esp_err_t error = connect_selected();
    if (error == ESP_OK) sync_calendar_clock();
    else terminal_network_release();
    return error;
}

esp_err_t terminal_network_acquire(void)
{
    if (!terminal_configured()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = terminal_network_acquire_link();
    if (error != ESP_OK) return error;
    if (!profile_sent) {
        char status[64];
        cJSON *reply = terminal_http_json(status, sizeof(status), HTTP_METHOD_POST,
                                          "/v1/devices/self", NULL, 200);
        profile_sent = reply != NULL;
        cJSON_Delete(reply);
        if (!profile_sent) {
            ESP_LOGW("terminal", "Device registration: %s", status);
            error = ESP_FAIL;
        }
    }
    if (error != ESP_OK) {
        terminal_network_release();
    }
    return error;
}
