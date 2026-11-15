#include "terminal_http.h"
#include "terminal_network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"

enum { MAX_RESPONSE = 128 * 1024 };

esp_http_client_handle_t terminal_http_client(const char *path, esp_http_client_method_t method,
                                              http_event_handle_cb handler, void *data)
{
    char url[640];
    int len = snprintf(url, sizeof(url), "%s%s", WINTERMUTE_SERVER_URL, path);
    if (len < 0 || (size_t)len >= sizeof(url))
        return NULL;
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .event_handler = handler,
        .user_data = data,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return NULL;
    if (esp_http_client_set_header(client, "X-Terminal-ID", WINTERMUTE_TERMINAL_ID) != ESP_OK ||
        esp_http_client_set_header(client, "Authorization", "Bearer " WINTERMUTE_TERMINAL_TOKEN) !=
            ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} response_buffer_t;

static esp_err_t receive_http(esp_http_client_event_t *event)
{
    response_buffer_t *buf = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || buf->failed) {
        return ESP_OK;
    }
    size_t needed = buf->len + (size_t)event->data_len + 1;
    if (needed > MAX_RESPONSE) {
        buf->failed = true;
        return ESP_FAIL;
    }
    if (needed > buf->cap) {
        size_t cap = buf->cap ? buf->cap * 2 : 1024;
        if (cap < needed)
            cap = needed;
        if (cap > MAX_RESPONSE)
            cap = MAX_RESPONSE;
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
    response_buffer_t buffer = {0};
    cJSON *json = NULL;
    char *encoded = body ? cJSON_PrintUnformatted(body) : NULL;
    esp_http_client_handle_t http = terminal_http_client(path, method, receive_http, &buffer);
    esp_err_t error = http && (!body || encoded) ? ESP_OK : ESP_ERR_NO_MEM;
    if (body && error == ESP_OK)
        error = esp_http_client_set_header(http, "Content-Type", "application/json");
    if (encoded && error == ESP_OK)
        error = esp_http_client_set_post_field(http, encoded, strlen(encoded));
    if (error == ESP_OK)
        error = esp_http_client_perform(http);
    int status = http ? esp_http_client_get_status_code(http) : 0;
    if (buffer.failed) {
        strlcpy(message, "Response too large for device", size);
    } else if (error != ESP_OK) {
        strlcpy(message, "Network error. Try again", size);
    } else if (status != expected) {
        snprintf(message, size, "Server returned HTTP %d", status);
    } else if (!buffer.data || !(json = cJSON_Parse(buffer.data)) || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        json = NULL;
        strlcpy(message, "Invalid server response", size);
    }
    if (http)
        esp_http_client_cleanup(http);
    cJSON_free(encoded);
    free(buffer.data);
    return json;
}
