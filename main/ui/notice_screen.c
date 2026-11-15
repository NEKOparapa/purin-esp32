#include "notice_screen.h"
#include "agent_screen.h"
#include "home_screen.h"
#include "notice_file.h"
#include "recorder.h"
#include "terminal_http.h"
#include "terminal_network.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef enum { CHECK_HOME, OPEN_PAGE, MARK_READ, SAVE_FILE } action_t;
typedef struct {
    action_t action;
    bool unread;
    char cursor[32];
    char notice_id[80];
    char file_id[80];
    char name[512];
    int64_t size;
} command_t;

typedef struct {
    action_t action;
    char status[128];
    cJSON *page;
    bool ok;
} result_t;

static QueueHandle_t commands, results;
static lv_obj_t *home, *home_label, *preview, *screen, *content, *status_label, *older_button,
    *read_button;
static cJSON *displayed;
static char current_cursor[32];
static uint32_t last_poll;
static bool busy, open_requested, home_unread, unread_view;

static const char *string(const cJSON *item, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, key));
}

static bool valid_id(const char *s)
{
    return s && *s && strlen(s) < 80 &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
               strlen(s);
}

static cJSON *first(cJSON *page)
{
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(page, "items"), 0);
}

static bool receipt(const char *id, const char *kind, char *status, size_t size)
{
    char route[140];
    snprintf(route, sizeof(route), "/v1/notifications/%s/%s", id, kind);
    cJSON *json = terminal_http_json(status, size, HTTP_METHOD_POST, route, NULL, 200);
    bool ok = json != NULL;
    cJSON_Delete(json);
    return ok;
}

static void fetch_page(const command_t *cmd, result_t *out)
{
    char route[120];
    snprintf(route, sizeof(route), "/v1/notifications?limit=1%s%s%s",
             cmd->action == CHECK_HOME || cmd->unread ? "&unread=1" : "",
             cmd->cursor[0] ? "&cursor=" : "", cmd->cursor);
    cJSON *page =
        terminal_http_json(out->status, sizeof(out->status), HTTP_METHOD_GET, route, NULL, 200);
    if (!page)
        return;
    cJSON *item = first(page);
    const char *cursor = string(page, "next_cursor");
    if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(page, "items")) ||
        (cursor && (!*cursor || strlen(cursor) >= sizeof(cmd->cursor) ||
                    strspn(cursor, "0123456789") != strlen(cursor))) ||
        (item &&
         (!valid_id(string(item, "id")) || !string(item, "title") || !string(item, "body") ||
          !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(item, "attachments"))))) {
        strlcpy(out->status, "Invalid notification page", sizeof(out->status));
        cJSON_Delete(page);
        return;
    }
    out->page = page;
    out->ok = true;
    if (item && cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(item, "received_at")))
        receipt(string(item, "id"), "received", out->status, sizeof(out->status));
}

static void worker(void *unused)
{
    (void)unused;
    command_t cmd;
    for (;;) {
        xQueueReceive(commands, &cmd, portMAX_DELAY);
        result_t out = {.action = cmd.action};
        if (terminal_network_acquire() != ESP_OK) {
            strlcpy(out.status, "Cannot connect to Wi-Fi", sizeof(out.status));
        } else {
            if (cmd.action == SAVE_FILE)
                notice_save_file(cmd.notice_id, cmd.file_id, cmd.name, cmd.size, out.status,
                                 sizeof(out.status));
            else if (cmd.action == MARK_READ) {
                out.ok = receipt(cmd.notice_id, "read", out.status, sizeof(out.status));
                if (out.ok)
                    strlcpy(out.status, "Marked as read", sizeof(out.status));
            } else
                fetch_page(&cmd, &out);
            terminal_network_release();
        }
        xQueueSend(results, &out, portMAX_DELAY);
    }
}

static bool start(const command_t *cmd)
{
    if (!commands || !terminal_configured() || busy)
        return false;
    busy = xQueueSend(commands, cmd, 0) == pdTRUE;
    return busy;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(obj, agent_text_font(), 0);
    lv_label_set_text(obj, text);
    return obj;
}

static void download(lv_event_t *event)
{
    if (busy)
        return;
    const cJSON *item = first(displayed), *file = lv_event_get_user_data(event);
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size_bytes");
    const char *id = string(file, "id"), *name = string(file, "name");
    if (!item || !valid_id(id) || !name || !cJSON_IsNumber(size) || size->valuedouble < 0 ||
        size->valuedouble > 8 * 1024 * 1024)
        return;
    command_t cmd = {.action = SAVE_FILE, .size = (int64_t)size->valuedouble};
    strlcpy(cmd.notice_id, string(item, "id"), sizeof(cmd.notice_id));
    strlcpy(cmd.file_id, id, sizeof(cmd.file_id));
    strlcpy(cmd.name, name, sizeof(cmd.name));
    if (start(&cmd))
        lv_label_set_text(status_label, "Saving attachment to SD...");
}

static void render(cJSON *page)
{
    lv_obj_clean(content);
    cJSON_Delete(displayed);
    displayed = page;
    cJSON *item = first(page);
    if (string(page, "next_cursor"))
        lv_obj_remove_state(older_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(older_button, LV_STATE_DISABLED);
    if (!item || string(item, "read_at"))
        lv_obj_add_state(read_button, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(read_button, LV_STATE_DISABLED);
    if (!item) {
        label(content, unread_view ? "No unread notifications" : "No notifications");
        return;
    }
    label(content, string(item, "title"));
    const char *when = string(item, "created_at");
    lv_label_set_text_fmt(label(content, ""), "%s   %s",
                          string(item, "read_at") ? "Read" : "Unread", when ? when : "");
    label(content, string(item, "body"));
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "expired")))
        label(content, "Expired");
    cJSON *files = cJSON_GetObjectItemCaseSensitive(item, "attachments"), *file;
    cJSON_ArrayForEach(file, files)
    {
        const char *name = string(file, "name");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size_bytes");
        if (!name || !valid_id(string(file, "id")) || !cJSON_IsNumber(size))
            continue;
        lv_obj_t *button = lv_button_create(content);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_t *caption = label(button, "");
        lv_label_set_text_fmt(caption, "%s (%u B)", name, (unsigned)size->valuedouble);
        lv_obj_add_event_cb(button, download, LV_EVENT_CLICKED, file);
    }
}

static void read_or_page(lv_event_t *event)
{
    if (busy)
        return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    command_t cmd = {.action = OPEN_PAGE};
    if (action == 'm') {
        const char *id = string(first(displayed), "id");
        if (!valid_id(id))
            return;
        cmd.action = MARK_READ;
        strlcpy(cmd.notice_id, id, sizeof(cmd.notice_id));
    } else if (action == 'o') {
        const char *cursor = string(displayed, "next_cursor");
        if (!cursor)
            return;
        strlcpy(cmd.cursor, cursor, sizeof(cmd.cursor));
        cmd.unread = unread_view;
    }
    if (start(&cmd)) {
        if (cmd.action == OPEN_PAGE) {
            strlcpy(current_cursor, cmd.cursor, sizeof(current_cursor));
            unread_view = cmd.unread;
        }
        lv_label_set_text(status_label, "Connecting...");
    }
}

static void to_home(lv_event_t *event)
{
    (void)event;
    notice_screen_hide();
}

void notice_screen_hide(void)
{
    open_requested = false;
    if (screen && lv_screen_active() == screen)
        lv_screen_load(home);
}

static lv_obj_t *button(lv_obj_t *bar, const char *text, lv_event_cb_t cb, int action)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_t *caption = lv_label_create(b);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    return b;
}

static void opened(lv_event_t *event)
{
    (void)event;
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *bar = lv_obj_create(screen);
        lv_obj_set_width(bar, LV_PCT(100));
        lv_obj_set_height(bar, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
        button(bar, "Home", to_home, 0);
        button(bar, "Latest", read_or_page, 'l');
        older_button = button(bar, "Older", read_or_page, 'o');
        read_button = button(bar, "Mark read", read_or_page, 'm');
        content = lv_obj_create(screen);
        lv_obj_set_width(content, LV_PCT(100));
        lv_obj_set_flex_grow(content, 1);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        status_label = label(screen, "");
    }
    lv_screen_load(screen);
    lv_label_set_text(status_label, commands && terminal_configured()
                                        ? "Loading..."
                                        : "Configure the device connection");
    open_requested = true;
    unread_view = home_unread;
    current_cursor[0] = 0;
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    result_t r;
    if (results && xQueueReceive(results, &r, 0) == pdTRUE) {
        busy = false;
        if (r.action == CHECK_HOME) {
            if (r.ok) {
                const char *title = string(first(r.page), "title");
                home_unread = title != NULL;
                const char *name = title ? "Notifications (unread)" : "Notifications";
                if (strcmp(name, lv_label_get_text(home_label)))
                    lv_label_set_text(home_label, name);
                if (strcmp(title ? title : "HOME", lv_label_get_text(preview))) {
                    lv_obj_set_style_text_font(
                        preview, title ? agent_text_font() : &lv_font_montserrat_48, 0);
                    lv_label_set_text(preview, title ? title : "HOME");
                }
            }
        } else if (r.action == MARK_READ && r.ok) {
            lv_obj_add_state(read_button, LV_STATE_DISABLED);
            open_requested = true;
        } else if (r.action == OPEN_PAGE && r.ok) {
            if (!displayed || !cJSON_Compare(r.page, displayed, true)) {
                render(r.page);
                r.page = NULL;
            }
        }
        if (r.action != CHECK_HOME && status_label)
            lv_label_set_text(status_label, r.status);
        cJSON_Delete(r.page);
    }
    if (busy || !commands || !terminal_configured())
        return;
    const recorder_state_t recording = recorder_get_status().state;
    const bool on_home = lv_screen_active() == home && !home_screen_upload_confirmation_visible() &&
                         (recording == RECORDER_IDLE || recording == RECORDER_ERROR);
    const bool on_latest = screen && lv_screen_active() == screen && !current_cursor[0];
    if (open_requested && lv_screen_active() == screen) {
        open_requested = false;
        last_poll = lv_tick_get();
        command_t cmd = {.action = OPEN_PAGE, .unread = unread_view};
        strlcpy(cmd.cursor, current_cursor, sizeof(cmd.cursor));
        start(&cmd);
    } else if ((on_home || on_latest) && lv_tick_elaps(last_poll) >= 60000) {
        last_poll = lv_tick_get();
        start(&(command_t){.action = on_home ? CHECK_HOME : OPEN_PAGE, .unread = unread_view});
    }
}

void notice_screen_init(lv_obj_t *home_button, lv_obj_t *home_heading)
{
    home = lv_screen_active();
    home_label = lv_obj_get_child(home_button, 0);
    preview = home_heading;
    lv_obj_add_event_cb(home_button, opened, LV_EVENT_CLICKED, NULL);
    commands = xQueueCreate(1, sizeof(command_t));
    results = xQueueCreate(1, sizeof(result_t));
    if (!commands || !results ||
        xTaskCreate(worker, "notifications", 12288, NULL, 1, NULL) != pdPASS) {
        if (commands)
            vQueueDelete(commands);
        if (results)
            vQueueDelete(results);
        commands = results = NULL;
        return;
    }
    last_poll = lv_tick_get() - 60000;
    lv_timer_create(tick, 250, NULL);
}
