#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef enum { APP_VIBRATION, APP_PLAYER, APP_CARD, APP_TIMER, APP_LIBRARY, APP_COUNT } device_app_t;
extern const char *const app_ids[APP_COUNT];
extern const char *const app_titles[APP_COUNT];
typedef struct {
    char id[80], title[193], body[961];
    bool acknowledged;
} prompt_card_t;
typedef enum {
    DESK_NONE, DESK_PULSE, DESK_BOOK, DESK_CHAPTER, DESK_PLAYBACK, DESK_STOPPED, DESK_ERROR
} desk_notice_kind_t;
typedef struct {
    desk_notice_kind_t kind;
    device_app_t app;
    char title[193];
    char book_id[13];
} desk_notice_t;
typedef struct {
    prompt_card_t card;
    desk_notice_t notice;
    device_app_t focus;
    const char *query_feedback;
    bool show_timer_end;
    char timer_label[193];
    int seconds_left, timer_seconds;
    bool expired;
} desk_state_t;

esp_err_t device_apps_init(void);
unsigned device_apps_mask(void);
esp_err_t device_apps_toggle(device_app_t app);
void device_apps_snapshot(desk_state_t *out);
bool device_apps_acknowledge(const char *id);
void device_apps_tick(void);
cJSON *device_apps_call(device_app_t app, const cJSON *args, const char *call_id,
                        char *error, size_t size);
const char *app_text(const cJSON *args, const char *key, size_t max_chars);
int app_number(const cJSON *args, const char *key, int min, int max, int fallback);
cJSON *app_error(char *error, size_t size, const char *message);
cJSON *app_result(const char *status);
cJSON *player_call(const cJSON *args, char *error, size_t size);
void player_stop(void);
void player_description(char *value, size_t size);
cJSON *library_call(const cJSON *args, char *error, size_t size);
