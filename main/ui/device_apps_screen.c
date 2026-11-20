#include "device_apps_screen.h"
#include "eink_ui.h"
#include "reader_screen.h"
#include "terminal_tools.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned generation;
    const char *action;
    int number;
    char key[65];
} media_job_t;

typedef struct {
    media_job_t job;
    cJSON *out;
    char error[96];
} media_result_t;

static lv_obj_t *screen, *back_screen, *body, *content, *detail, *caption, *message;
static lv_obj_t *confirm, *toggle_label;
static device_app_t current;
static char card_id[80], keys[4][65];
static unsigned generation;
static int offset;
static QueueHandle_t jobs, results;
static lv_timer_t *refresh;

static void set_text(lv_obj_t *obj, const char *value)
{
    if (obj && strcmp(lv_label_get_text(obj), value)) lv_label_set_text(obj, value);
}

static void feedback(const char *value)
{
    if (*value) {
        set_text(message, value);
        lv_obj_remove_flag(message, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_text(message, "");
        lv_obj_add_flag(message, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const char *s)
{
    lv_obj_t *text = eink_label(parent, s, eink_body_font(), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(text, LV_PCT(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    return text;
}

static lv_obj_t *button(lv_obj_t *parent, const char *s, lv_event_cb_t cb, intptr_t arg)
{
    lv_obj_t *b = eink_outline_button(parent, s, eink_body_font(), 56, cb, (void *)arg);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_t *title = lv_obj_get_child(b, 0);
    lv_obj_set_width(title, LV_PCT(96));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    return b;
}

static void go_back(lv_event_t *e)
{
    (void)e;
    ++generation;
    lv_screen_load(back_screen);
}

static void update_toggle(void)
{
    bool on = device_apps_mask() & (1u << current);
    set_text(toggle_label, on ? "远程已开启" : "远程已关闭");
}

static void toggle(lv_event_t *e)
{
    (void)e;
    esp_err_t err = device_apps_toggle(current);
    update_toggle();
    feedback(err != ESP_OK ? "设置保存失败" : terminal_tools_enabled() ?
             "" : "远程调用需在固件中配置 HTTPS 服务与设备凭据");
}

static void act(lv_event_t *e)
{
    const char *action = (const char *)lv_event_get_user_data(e);
    cJSON *args = cJSON_CreateObject();
    if (!args) return;
    if (current == APP_TIMER && strcmp(action, "stop")) {
        cJSON_AddStringToObject(args, "action", "start");
        cJSON_AddNumberToObject(args, "seconds", !strcmp(action, "focus") ? 1500 : 300);
        cJSON_AddStringToObject(args, "label", !strcmp(action, "focus") ? "专注" : "休息");
    } else cJSON_AddStringToObject(args, "action", action);
    char error[96] = "内存不足";
    cJSON *out = device_apps_call(current, args, NULL, error, sizeof(error));
    feedback(out ? (current == APP_VIBRATION ? "已执行" : "") : error);
    cJSON_Delete(out);
    cJSON_Delete(args);
}

static void ack(lv_event_t *e)
{
    (void)e;
    if (!device_apps_acknowledge(card_id)) feedback("卡片已更换或保存失败");
}

static void media_worker(void *unused)
{
    (void)unused;
    media_job_t job;
    for (;;) {
        xQueueReceive(jobs, &job, portMAX_DELAY);
        media_result_t result = {.job = job, .error = "内存不足"};
        cJSON *args = cJSON_CreateObject();
        if (args) {
            cJSON_AddStringToObject(args, "action", job.action);
            if (!strcmp(job.action, "list")) cJSON_AddNumberToObject(args, "offset", job.number);
            else cJSON_AddStringToObject(args, "file", job.key);
            result.out = player_call(args, result.error, sizeof(result.error));
            cJSON_Delete(args);
        }
        xQueueSend(results, &result, portMAX_DELAY);
    }
}

static void request(const char *action, int number, const char *key)
{
    if (!jobs) { feedback("存储任务不可用"); return; }
    media_job_t job = {.generation = ++generation, .action = action, .number = number};
    strlcpy(job.key, key ? key : "", sizeof(job.key));
    xQueueOverwrite(jobs, &job);
    feedback("正在读取");
}

static void browse(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    request("list", delta ? offset + delta : 0, NULL);
}

static void select_entry(lv_event_t *e)
{
    unsigned i = (uintptr_t)lv_event_get_user_data(e);
    request("play", 0, keys[i]);
}

static void render_media(media_result_t *res)
{
    if (!res->out) { feedback(res->error); return; }
    feedback("");
    if (!strcmp(res->job.action, "play")) return;
    lv_obj_clean(content);
    offset = res->job.number;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(res->out, "items");
    int count = cJSON_GetArraySize(items);
    if (count == 0) label(content, "Audio 目录暂无 WAV 文件");
    for (int i = 0; i < count && i < 4; i++) {
        const char *key = cJSON_GetStringValue(cJSON_GetArrayItem(items, i));
        strlcpy(keys[i], key, sizeof(keys[i]));
        button(content, key, select_entry, i);
    }
    if (offset) button(content, "上一组", browse, -4);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res->out, "has_more"))) button(content, "下一组", browse, 4);
    lv_obj_scroll_to_view(content, LV_ANIM_OFF);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    bool active = screen && lv_screen_active() == screen;
    media_result_t res;
    if (results && xQueueReceive(results, &res, 0) == pdTRUE) {
        if (active && res.job.generation == generation) render_media(&res);
        cJSON_Delete(res.out);
    }
    if (!active) return;
    if (current == APP_CARD || current == APP_TIMER) {
        desk_state_t desk;
        device_apps_snapshot(&desk);
        if (current == APP_CARD) {
            strlcpy(card_id, desk.card.id, sizeof(card_id));
            set_text(caption, card_id[0] ? desk.card.title : "暂无提示卡片");
            set_text(detail, desk.card.body);
            bool hide = !card_id[0] || desk.card.acknowledged;
            if (hide) lv_obj_add_flag(confirm, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(confirm, LV_OBJ_FLAG_HIDDEN);
            if (desk.card.acknowledged) feedback("已确认");
            else if (!strcmp(lv_label_get_text(message), "已确认")) feedback("");
        } else {
            char value[48];
            if (desk.seconds_left >= 60)
                snprintf(value, sizeof(value), "剩余 %d 分钟", (desk.seconds_left + 59) / 60);
            else if (desk.seconds_left)
                snprintf(value, sizeof(value), "剩余 %d 秒", desk.seconds_left);
            else strlcpy(value, desk.expired ? "时间到了" : "尚未开始", sizeof(value));
            set_text(caption, desk.timer_label);
            set_text(detail, value);
        }
    } else if (current == APP_PLAYER) {
        char value[96];
        player_description(value, sizeof(value));
        set_text(detail, value);
    }
}

static void open_app(device_app_t app, lv_obj_t *back)
{
    if (!screen) screen = lv_obj_create(NULL);
    lv_obj_clean(screen);
    ++generation;
    current = app;
    back_screen = back;
    eink_style_screen(screen);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *slot;
    eink_top_bar_font(screen, app_titles[app], eink_heading_font(), go_back, &slot);
    lv_obj_t *remote = eink_outline_button(slot, "", eink_small_font(), 42, toggle, NULL);
    lv_obj_set_width(remote, 132);
    toggle_label = lv_obj_get_child(remote, 0);
    update_toggle();
    body = lv_obj_create(screen);
    eink_style_clean_box(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, EINK_PAD_X, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    message = label(body, "");
    feedback("");
    if (app == APP_VIBRATION) {
        button(body, "短震一次", act, (intptr_t)"pulse");
        button(body, "停止震动", act, (intptr_t)"stop");
    } else if (app == APP_CARD) {
        lv_obj_t *panel = lv_obj_create(body);
        eink_style_clean_box(panel);
        eink_style_card(panel, EINK_RADIUS_CARD, EINK_BORDER_WIDTH);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(panel, 20, 0);
        lv_obj_set_style_pad_row(panel, 14, 0);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        caption = label(panel, "");
        detail = label(panel, "");
        lv_obj_set_style_text_font(caption, eink_heading_font(), 0);
        lv_obj_set_style_text_line_space(detail, 7, 0);
        confirm = button(body, "我看过了", ack, 0);
    } else if (app == APP_TIMER) {
        caption = label(body, ""); detail = label(body, "");
        lv_obj_set_style_text_font(detail, eink_heading_font(), 0);
        button(body, "专注 25 分钟", act, (intptr_t)"focus");
        button(body, "休息 5 分钟", act, (intptr_t)"break");
        button(body, "结束计时", act, (intptr_t)"stop");
    } else {
        if (!jobs) {
            jobs = xQueueCreate(1, sizeof(media_job_t));
            results = xQueueCreate(1, sizeof(media_result_t));
            if (!jobs || !results || xTaskCreate(media_worker, "app_files", 6144, NULL, 1, NULL) != pdPASS) {
                if (jobs) vQueueDelete(jobs);
                if (results) vQueueDelete(results);
                jobs = NULL; results = NULL;
            }
        }
        detail = label(body, "");
        button(body, "播放提示音", act, (intptr_t)"play");
        button(body, "停止播放", act, (intptr_t)"stop");
        button(body, "浏览 SD 卡音频", browse, 0);
        content = lv_obj_create(body);
        eink_style_clean_box(content);
        lv_obj_set_width(content, LV_PCT(100));
        lv_obj_set_height(content, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(content, 12, 0);
    }
    lv_screen_load(screen);
    if (!refresh) refresh = lv_timer_create(tick, 400, NULL);
    tick(NULL);
}

void device_app_open(device_app_t app, lv_obj_t *back)
{
    if (app == APP_LIBRARY) reader_open(back, NULL);
    else open_app(app, back);
}

void device_book_open(const char *id, lv_obj_t *back)
{
    reader_open(back, id);
}
