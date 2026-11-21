#include "terminal_pairing.h"
#include "terminal_http.h"
#include "terminal_network.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bootloader_random.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

typedef struct {
    char server[TERMINAL_SERVER_SIZE];
    char device_id[31];
    char token[TERMINAL_TOKEN_SIZE];
} paired_connection_t;

static paired_connection_t paired;
static paired_connection_t pending;
static char pending_code[11];
static unsigned char pairing_seed[32];
static const char pairing_namespace[] = "wm_pairing";

static bool valid_id(const char *id)
{
    return id && strlen(id) == 30 && !strncmp(id, "esp32-", 6) &&
           strspn(id + 6, "0123456789abcdef") == 24;
}

static bool valid_token(const char *token)
{
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    return strlen(token) == TERMINAL_TOKEN_SIZE - 1 && strspn(token, alphabet) == strlen(token);
}

static bool normalize_server(const char *address, char server[TERMINAL_SERVER_SIZE])
{
    const char *authority = !strncmp(address, "https://", 8) ? address + 8 : address;
    size_t length = strcspn(authority, "/");
    if (!length || length + 8 >= TERMINAL_SERVER_SIZE ||
        (authority[length] && strcmp(authority + length, "/")) ||
        strspn(authority, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:[]") != length) {
        return false;
    }
    snprintf(server, TERMINAL_SERVER_SIZE, "https://%.*s", (int)length, authority);
    return true;
}

void terminal_load_pairing(void)
{
    nvs_handle_t preferences;
    if (nvs_open(pairing_namespace, NVS_READONLY, &preferences) != ESP_OK) return;
    size_t size = sizeof(paired);
    esp_err_t error = nvs_get_blob(preferences, "connection", &paired, &size);
    nvs_close(preferences);
    if (error != ESP_OK || size != sizeof(paired) ||
        paired.server[sizeof(paired.server) - 1] || paired.token[sizeof(paired.token) - 1] ||
        paired.device_id[sizeof(paired.device_id) - 1] || !valid_id(paired.device_id) ||
        strncmp(paired.server, "https://", 8) || !valid_token(paired.token)) {
        mbedtls_platform_zeroize(&paired, sizeof(paired));
    }
}

bool terminal_is_paired(void) { return paired.device_id[0] != '\0'; }
const char *terminal_server_url(void) { return terminal_is_paired() ? paired.server : WINTERMUTE_SERVER_URL; }
const char *terminal_device_token(void) { return terminal_is_paired() ? paired.token : WINTERMUTE_DEVICE_TOKEN; }

const char *wintermute_device_id(void)
{
    if (terminal_is_paired()) return paired.device_id;
    return WINTERMUTE_DEVICE_ID[0] ? WINTERMUTE_DEVICE_ID : wintermute_device_instance_id();
}

void terminal_seed_pairing(void)
{
    // The cellular path does not enable Wi-Fi's hardware entropy source.
    bootloader_random_enable();
    esp_fill_random(pairing_seed, sizeof(pairing_seed));
    bootloader_random_disable();
}

static bool new_token(char token[TERMINAL_TOKEN_SIZE])
{
    static uint32_t count;
    ++count;
    unsigned char bytes[32], encoded[45];
    size_t length;
    const mbedtls_md_info_t *sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(sha, pairing_seed, sizeof(pairing_seed),
                        (const unsigned char *)&count, sizeof(count), bytes) != 0) return false;
    int error = mbedtls_base64_encode(encoded, sizeof(encoded), &length, bytes, sizeof(bytes));
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    if (error != 0 || length != 44) return false;
    for (size_t i = 0; i < 43; ++i) {
        token[i] = encoded[i] == '+' ? '-' : encoded[i] == '/' ? '_' : (char)encoded[i];
    }
    token[43] = '\0';
    mbedtls_platform_zeroize(encoded, sizeof(encoded));
    return true;
}

static esp_err_t save_connection(void)
{
    nvs_handle_t preferences;
    esp_err_t error = nvs_open(pairing_namespace, NVS_READWRITE, &preferences);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(preferences, "connection", &pending, sizeof(pending));
    if (error == ESP_OK) error = nvs_commit(preferences);
    nvs_close(preferences);
    return error;
}

bool terminal_pair_device(const char *address, const char *code, char *message, size_t size)
{
    char server[TERMINAL_SERVER_SIZE];
    if (!normalize_server(address, server)) {
        strlcpy(message, "请输入 HTTPS 服务域名。", size);
        return false;
    }
    if (strlen(code) != 10 || strspn(code, "0123456789") != 10) {
        strlcpy(message, "请输入 10 位配对码。", size);
        return false;
    }
    if (terminal_network_acquire_link() != ESP_OK) {
        strlcpy(message, "网络未连接，请检查网络设置。", size);
        return false;
    }
    // A retry keeps the same credential, including after an interrupted response.
    if (strcmp(pending.server, server) || strcmp(pending_code, code) || !pending.token[0]) {
        strlcpy(pending.server, server, sizeof(pending.server));
        strlcpy(pending_code, code, sizeof(pending_code));
        if (!new_token(pending.token)) {
            terminal_network_release();
            strlcpy(message, "无法生成设备凭据。", size);
            return false;
        }
    }
    cJSON *body = cJSON_CreateObject();
    int status = 0;
    cJSON *reply = NULL;
    if (cJSON_AddStringToObject(body, "code", pending_code) &&
        cJSON_AddStringToObject(body, "token", pending.token) &&
        cJSON_AddStringToObject(body, "installation", wintermute_device_instance_id()) &&
        cJSON_AddStringToObject(body, "device_type", "esp32") &&
        cJSON_AddStringToObject(body, "device_name", WINTERMUTE_DEVICE_NAME)) {
        reply = terminal_http_redeem_pairing(pending.server, body, &status);
    }
    cJSON_Delete(body);
    terminal_network_release();
    const char *device_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "device_id"));
    bool valid = valid_id(device_id);
    if (valid) strlcpy(pending.device_id, device_id, sizeof(pending.device_id));
    cJSON_Delete(reply);
    if (!valid) {
        const char *error = "连接失败，请检查服务地址后重试。";
        if (status == 403) error = "配对码无效、已过期或已使用。";
        if (status == 404) error = "后端尚未启用设备配对。";
        if (status == 429) error = "尝试次数过多，请稍后再试。";
        strlcpy(message, error, size);
        return false;
    }
    if (save_connection() != ESP_OK) {
        strlcpy(message, "无法保存设备连接，请重试。", size);
        return false;
    }
    mbedtls_platform_zeroize(&pending, sizeof(pending));
    mbedtls_platform_zeroize(pending_code, sizeof(pending_code));
    return true;
}
