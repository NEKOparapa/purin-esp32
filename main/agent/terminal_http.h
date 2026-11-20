#pragma once
#include <stddef.h>
#include "cJSON.h"
#include "esp_http_client.h"
esp_err_t terminal_set_auth_headers(esp_http_client_handle_t client);

esp_http_client_handle_t terminal_http_client(const char *path, esp_http_client_method_t method,
                                              http_event_handle_cb handler, void *data);
cJSON *terminal_http_json(char *status, size_t size, esp_http_client_method_t method,
                          const char *path, const cJSON *body, int expected);
cJSON *terminal_http_json_bounded(char *message, size_t size, esp_http_client_method_t method,
                                  const char *path, const cJSON *body, int expected,
                                  size_t limit, int *status_code);
