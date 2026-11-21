#include "terminal_http.h"
#include "terminal_network.h"
#include "terminal_pairing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

enum { MAX_RESPONSE = 128 * 1024 };

esp_err_t terminal_set_auth_headers(esp_http_client_handle_t client)
{
    const unsigned char name[] = WINTERMUTE_DEVICE_NAME;
    unsigned char encoded[4 * ((sizeof(name) + 1) / 3) + 1];
    size_t length;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &length, name, sizeof(name) - 1) != 0)
        return ESP_ERR_INVALID_ARG;
    encoded[length] = 0;
    esp_err_t rc = esp_http_client_set_header(client, "X-Device-ID", wintermute_device_id());
    if (rc == ESP_OK) rc = esp_http_client_set_header(client, "X-Device-Instance", wintermute_device_instance_id());
    if (rc == ESP_OK) rc = esp_http_client_set_header(client, "X-Device-Name", (const char *)encoded);
    if (rc == ESP_OK) rc = esp_http_client_set_header(client, "X-Device-Platform", WINTERMUTE_DEVICE_PLATFORM);
    char auth[sizeof("Bearer ") + sizeof(WINTERMUTE_DEVICE_TOKEN) + TERMINAL_TOKEN_SIZE];
    snprintf(auth, sizeof(auth), "Bearer %s", terminal_device_token());
    if (rc == ESP_OK) rc = esp_http_client_set_header(client, "Authorization", auth);
    mbedtls_platform_zeroize(auth, sizeof(auth));
    return rc;
}

static esp_http_client_handle_t make_http_client(const char *url, esp_http_client_method_t method,
                                                 http_event_handle_cb handler, void *data)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .event_handler = handler,
        .user_data = data,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    return esp_http_client_init(&config);
}

esp_http_client_handle_t terminal_http_client(const char *path, esp_http_client_method_t method,
                                              http_event_handle_cb handler, void *data)
{
    char url[640];
    int len = snprintf(url, sizeof(url), "%s%s", terminal_server_url(), path);
    if (len < 0 || (size_t)len >= sizeof(url)) return NULL;
    esp_http_client_handle_t client = make_http_client(url, method, handler, data);
    if (!client)
        return NULL;
    if (terminal_set_auth_headers(client) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    bool failed;
} response_buffer_t;

static esp_err_t receive_http(esp_http_client_event_t *event)
{
    response_buffer_t *buf = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || buf->failed) {
        return ESP_OK;
    }
    size_t needed = buf->len + (size_t)event->data_len + 1;
    if (needed > buf->limit) {
        buf->failed = true;
        return ESP_FAIL;
    }
    if (needed > buf->cap) {
        size_t cap = buf->cap ? buf->cap * 2 : 1024;
        if (cap < needed)
            cap = needed;
        if (cap > buf->limit)
            cap = buf->limit;
        char *next = realloc(buf->data, cap);
        if (!next) {
            buf->failed = true;
            return ESP_ERR_NO_MEM;
        }
        buf->data = next;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, event->data, event->data_len);
    buf->len += (size_t)event->data_len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

cJSON *terminal_http_json(char *message, size_t size, esp_http_client_method_t method,
                          const char *path, const cJSON *body, int expected)
{
    return terminal_http_json_bounded(message, size, method, path, body, expected, MAX_RESPONSE, NULL);
}

static cJSON *read_http_json(esp_http_client_handle_t http, response_buffer_t *buffer,
                             char *message, size_t size, const cJSON *body,
                             int expected, int *status_code)
{
    cJSON *json = NULL;
    char *encoded = body ? cJSON_PrintUnformatted(body) : NULL;
    esp_err_t error = http && (!body || encoded) ? ESP_OK : ESP_ERR_NO_MEM;
    if (body && error == ESP_OK)
        error = esp_http_client_set_header(http, "Content-Type", "application/json");
    if (encoded && error == ESP_OK)
        error = esp_http_client_set_post_field(http, encoded, strlen(encoded));
    if (error == ESP_OK)
        error = esp_http_client_perform(http);
    int status = http ? esp_http_client_get_status_code(http) : 0;
    if (status_code) *status_code = status;
    if (buffer->failed) {
        strlcpy(message, "Response too large for device", size);
    } else if (error != ESP_OK) {
        strlcpy(message, "Network error. Try again", size);
    } else if (status != expected) {
        snprintf(message, size, "Server returned HTTP %d", status);
    } else if (!buffer->data || !(json = cJSON_Parse(buffer->data)) || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        json = NULL;
        strlcpy(message, "Invalid server response", size);
    }
    if (http)
        esp_http_client_cleanup(http);
    if (encoded) mbedtls_platform_zeroize(encoded, strlen(encoded));
    cJSON_free(encoded);
    free(buffer->data);
    return json;
}

cJSON *terminal_http_json_bounded(char *message, size_t size, esp_http_client_method_t method,
                                  const char *path, const cJSON *body, int expected,
                                  size_t limit, int *status_code)
{
    response_buffer_t buffer = {.limit = limit};
    esp_http_client_handle_t http = terminal_http_client(path, method, receive_http, &buffer);
    return read_http_json(http, &buffer, message, size, body, expected, status_code);
}

cJSON *terminal_http_redeem_pairing(const char *server, const cJSON *body, int *status_code)
{
    const char route[] = "/v1/devices/pair";
    char url[TERMINAL_SERVER_SIZE + sizeof(route)];
    snprintf(url, sizeof(url), "%s%s", server, route);
    response_buffer_t buffer = {.limit = 4096};
    esp_http_client_handle_t http = make_http_client(url, HTTP_METHOD_POST, receive_http, &buffer);
    char message[72];
    return read_http_json(http, &buffer, message, sizeof(message), body, 200, status_code);
}
