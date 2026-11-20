#include "question_card.h"
#include "eink_ui.h"
#include "terminal_http.h"
#include "terminal_network.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MAX_QUESTION_OPTIONS 6
#define MAX_OPTION_TEXT 128
#define MAX_ANSWER_TEXT 512

typedef enum { Q_FETCH, Q_SUBMIT_ANSWER, Q_DISMISS } q_action_t;

typedef struct {
    q_action_t action;
    char cursor[32];
    char question_id[80];
    int selected_count;
    char selected_options[MAX_QUESTION_OPTIONS][MAX_OPTION_TEXT];
    char answer_text[MAX_ANSWER_TEXT];
} q_command_t;

typedef struct {
    q_action_t action;
    char status[128];
    cJSON *page;
    bool ok;
} q_result_t;

static QueueHandle_t commands, results;
static lv_obj_t *chat_screen_ref;
static lv_obj_t *card;
static lv_obj_t *option_buttons[MAX_QUESTION_OPTIONS];
static lv_obj_t *custom_input;
static question_card_layout_cb_t layout_cb;
static question_card_focus_cb_t focus_cb;
static question_card_status_cb_t status_cb;
static cJSON *displayed;
static char current_cursor[32];
static uint32_t last_poll;
static bool busy, fetch_requested, current_multi_select, has_item;
static int option_count;

static const char *q_string(const cJSON *item, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, key));
}

static bool q_valid_id(const char *s)
{
    return s && *s && strlen(s) < 80 &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
               strlen(s);
}

static cJSON *q_first(cJSON *page)
{
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(page, "items"), 0);
}

static void emit_status(const char *text)
{
    if (status_cb && text && *text) status_cb(text);
}

static void notify_layout(void)
{
    if (layout_cb) layout_cb();
}

static void fetch_page(const q_command_t *cmd, q_result_t *out)
{
    char route[128];
    snprintf(route, sizeof(route), "/v1/questions?status=pending&limit=1%s%s",
             cmd->cursor[0] ? "&cursor=" : "", cmd->cursor);
    int code = 0;
    cJSON *page = terminal_http_json_bounded(out->status, sizeof(out->status), HTTP_METHOD_GET,
                                             route, NULL, 200, 16384, &code);
    if (!page)
        return;
    cJSON *item = q_first(page);
    const char *cursor = q_string(page, "next_cursor");
    if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(page, "items")) ||
        (cursor && (!*cursor || strlen(cursor) >= sizeof(cmd->cursor) ||
                    strspn(cursor, "0123456789") != strlen(cursor))) ||
        (item && (!q_valid_id(q_string(item, "id")) || !q_string(item, "title") ||
                  !q_string(item, "question")))) {
        strlcpy(out->status, "Invalid question page", sizeof(out->status));
        cJSON_Delete(page);
        return;
    }
    out->page = page;
    out->ok = true;
}

static void post_answer(const q_command_t *cmd, q_result_t *out)
{
    char route[140];
    snprintf(route, sizeof(route), "/v1/questions/%s/answer", cmd->question_id);
    cJSON *body = cJSON_CreateObject();
    cJSON *selected = cJSON_AddArrayToObject(body, "selected_options");
    for (int i = 0; i < cmd->selected_count && i < MAX_QUESTION_OPTIONS; ++i)
        cJSON_AddItemToArray(selected, cJSON_CreateString(cmd->selected_options[i]));
    cJSON_AddStringToObject(body, "answer_text", cmd->answer_text);
    cJSON *resp = terminal_http_json(out->status, sizeof(out->status), HTTP_METHOD_POST, route,
                                     body, 200);
    cJSON_Delete(body);
    if (resp) {
        cJSON_Delete(resp);
        out->ok = true;
        strlcpy(out->status, "已提交回答", sizeof(out->status));
    }
}

static void post_dismiss(const q_command_t *cmd, q_result_t *out)
{
    char route[140];
    snprintf(route, sizeof(route), "/v1/questions/%s/dismiss", cmd->question_id);
    cJSON *body = cJSON_CreateObject();
    cJSON *resp = terminal_http_json(out->status, sizeof(out->status), HTTP_METHOD_POST, route,
                                     body, 200);
    cJSON_Delete(body);
    if (resp) {
        cJSON_Delete(resp);
        out->ok = true;
        strlcpy(out->status, "已忽略该提问", sizeof(out->status));
    }
}

static void worker(void *unused)
{
    (void)unused;
    q_command_t cmd;
    for (;;) {
        xQueueReceive(commands, &cmd, portMAX_DELAY);
        q_result_t out = {.action = cmd.action};
        if (terminal_network_acquire() != ESP_OK) {
            strlcpy(out.status, "Cannot connect to Wi-Fi", sizeof(out.status));
        } else {
            if (cmd.action == Q_SUBMIT_ANSWER)
                post_answer(&cmd, &out);
            else if (cmd.action == Q_DISMISS)
                post_dismiss(&cmd, &out);
            else
                fetch_page(&cmd, &out);
            terminal_network_release();
        }
        xQueueSend(results, &out, portMAX_DELAY);
    }
}

static bool start_cmd(const q_command_t *cmd)
{
    if (!commands || !terminal_configured() || busy)
        return false;
    busy = xQueueSend(commands, cmd, 0) == pdTRUE;
    return busy;
}

static void style_option_chip(lv_obj_t *b)
{
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_color(b, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(b, 10, LV_PART_MAIN);

    lv_obj_set_style_bg_color(b, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_PRESSED);
}

static void option_clicked(lv_event_t *event)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(event);
    if (idx < 0 || idx >= option_count)
        return;
    if (current_multi_select) {
        if (lv_obj_has_state(option_buttons[idx], LV_STATE_CHECKED))
            lv_obj_remove_state(option_buttons[idx], LV_STATE_CHECKED);
        else
            lv_obj_add_state(option_buttons[idx], LV_STATE_CHECKED);
    } else {
        bool was_checked = lv_obj_has_state(option_buttons[idx], LV_STATE_CHECKED);
        for (int i = 0; i < option_count; ++i)
            lv_obj_remove_state(option_buttons[i], LV_STATE_CHECKED);
        if (!was_checked)
            lv_obj_add_state(option_buttons[idx], LV_STATE_CHECKED);
    }
}

static void submit_clicked(lv_event_t *event)
{
    (void)event;
    if (busy)
        return;
    const cJSON *item = q_first(displayed);
    const char *id = q_string(item, "id");
    if (!q_valid_id(id))
        return;

    q_command_t cmd = {.action = Q_SUBMIT_ANSWER};
    strlcpy(cmd.question_id, id, sizeof(cmd.question_id));

    cJSON *options = cJSON_GetObjectItemCaseSensitive(item, "options");
    for (int i = 0; i < option_count && i < MAX_QUESTION_OPTIONS; ++i) {
        if (lv_obj_has_state(option_buttons[i], LV_STATE_CHECKED)) {
            const char *opt = cJSON_GetStringValue(cJSON_GetArrayItem(options, i));
            if (opt && *opt) {
                strlcpy(cmd.selected_options[cmd.selected_count], opt, MAX_OPTION_TEXT);
                cmd.selected_count++;
            }
        }
    }
    if (custom_input) {
        const char *txt = lv_textarea_get_text(custom_input);
        if (txt)
            strlcpy(cmd.answer_text, txt, sizeof(cmd.answer_text));
    }
    if (cmd.selected_count == 0 && cmd.answer_text[0] == '\0') {
        emit_status("请先选择选项或填写回答");
        return;
    }
    if (start_cmd(&cmd))
        emit_status("正在提交回答...");
}

static void card_action(lv_event_t *event)
{
    if (busy)
        return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    q_command_t cmd = {.action = Q_FETCH};
    if (action == 'd') {
        const char *id = q_string(q_first(displayed), "id");
        if (!q_valid_id(id))
            return;
        cmd.action = Q_DISMISS;
        strlcpy(cmd.question_id, id, sizeof(cmd.question_id));
    } else if (action == 'n') {
        const char *cursor = q_string(displayed, "next_cursor");
        if (!cursor)
            return;
        strlcpy(cmd.cursor, cursor, sizeof(cmd.cursor));
    }
    if (start_cmd(&cmd)) {
        if (cmd.action == Q_FETCH)
            strlcpy(current_cursor, cmd.cursor, sizeof(current_cursor));
        emit_status(action == 'd' ? "正在忽略提问..." : "正在加载下一条...");
    }
}

static void input_clicked(lv_event_t *event)
{
    (void)event;
    if (focus_cb && custom_input) focus_cb(custom_input);
}

static void render_card(cJSON *page)
{
    lv_obj_clean(card);
    cJSON_Delete(displayed);
    displayed = page;
    option_count = 0;
    custom_input = NULL;

    cJSON *item = q_first(page);
    if (!item) {
        has_item = false;
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
        notify_layout();
        return;
    }

    has_item = true;
    current_multi_select = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "multi_select"));
    bool allow_custom = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(item, "allow_custom"));
    cJSON *options = cJSON_GetObjectItemCaseSensitive(item, "options");
    int total_opts = cJSON_IsArray(options) ? cJSON_GetArraySize(options) : 0;
    const char *next_cursor = q_string(page, "next_cursor");

    // Header Row: Badge + Next + Dismiss
    lv_obj_t *hdr = lv_obj_create(card);
    eink_style_clean_box(hdr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    eink_badge(hdr,
               total_opts > 0 ? (current_multi_select ? "待回答 · 多选" : "待回答 · 单选")
                              : "待回答",
               eink_small_font(), true);

    lv_obj_t *hdr_actions = lv_obj_create(hdr);
    eink_style_clean_box(hdr_actions);
    lv_obj_remove_flag(hdr_actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hdr_actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(hdr_actions, 6, LV_PART_MAIN);

    if (next_cursor && *next_cursor) {
        lv_obj_t *next_btn = eink_pill_button(hdr_actions, "下一条", eink_small_font(), 32, false,
                                              card_action, (void *)(intptr_t)'n');
        lv_obj_set_style_pad_hor(next_btn, 10, LV_PART_MAIN);
    }
    lv_obj_t *dismiss_btn = eink_pill_button(hdr_actions, "忽略", eink_small_font(), 32, false,
                                             card_action, (void *)(intptr_t)'d');
    lv_obj_set_style_pad_hor(dismiss_btn, 10, LV_PART_MAIN);

    // Scrollable inner body for title, question, options and optional input
    lv_obj_t *body = lv_obj_create(card);
    eink_style_clean_box(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(body, 224, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_row(body, 8, LV_PART_MAIN);

    lv_obj_t *title_lbl = eink_label(body, q_string(item, "title"), eink_body_font(),
                                     LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(title_lbl, LV_PCT(100));
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_MODE_WRAP);

    const char *q_text = q_string(item, "question");
    if (q_text && *q_text) {
        lv_obj_t *q_lbl = eink_label(body, q_text, eink_small_font(), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(q_lbl, LV_PCT(100));
        lv_label_set_long_mode(q_lbl, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(q_lbl, 4, LV_PART_MAIN);
    }

    for (int i = 0; i < total_opts && i < MAX_QUESTION_OPTIONS; ++i) {
        const char *opt = cJSON_GetStringValue(cJSON_GetArrayItem(options, i));
        if (!opt || !*opt)
            continue;
        lv_obj_t *b = lv_button_create(body);
        style_option_chip(b);
        lv_obj_t *opt_lbl = lv_label_create(b);
        lv_obj_set_width(opt_lbl, LV_PCT(100));
        lv_label_set_long_mode(opt_lbl, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(opt_lbl, eink_small_font(), LV_PART_MAIN);
        lv_label_set_text(opt_lbl, opt);
        lv_obj_add_event_cb(b, option_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)option_count);
        option_buttons[option_count++] = b;
    }

    if (allow_custom || option_count == 0) {
        lv_obj_t *input_frame = eink_textarea(body, eink_small_font(), 46, EINK_RADIUS_CONTROL,
                                             &custom_input);
        lv_obj_set_width(input_frame, LV_PCT(100));
        lv_textarea_set_max_length(custom_input, MAX_ANSWER_TEXT - 1);
        lv_textarea_set_placeholder_text(
            custom_input, option_count > 0 ? "补充说明（可选）..." : "点击输入回答...");
        lv_obj_add_event_cb(custom_input, input_clicked, LV_EVENT_CLICKED, NULL);
    }

    // Submit CTA
    lv_obj_t *submit = eink_pill_button(card, "提交回答", eink_small_font(), 44, true,
                                        submit_clicked, NULL);
    lv_obj_set_width(submit, LV_PCT(100));

    lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(card);
    notify_layout();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    q_result_t r;
    if (results && xQueueReceive(results, &r, 0) == pdTRUE) {
        busy = false;
        if ((r.action == Q_SUBMIT_ANSWER || r.action == Q_DISMISS) && r.ok) {
            current_cursor[0] = 0;
            fetch_requested = true;
            emit_status(r.status);
        } else if (r.action == Q_FETCH && r.ok) {
            if (!displayed || !cJSON_Compare(r.page, displayed, true)) {
                render_card(r.page);
                r.page = NULL;
            }
        } else if (!r.ok && r.action != Q_FETCH) {
            emit_status(r.status);
        }
        cJSON_Delete(r.page);
    }
    if (busy || !commands || !terminal_configured())
        return;
    const bool on_chat = chat_screen_ref && lv_screen_active() == chat_screen_ref;
    if (!on_chat)
        return;
    if (fetch_requested) {
        fetch_requested = false;
        last_poll = lv_tick_get();
        q_command_t cmd = {.action = Q_FETCH};
        strlcpy(cmd.cursor, current_cursor, sizeof(cmd.cursor));
        start_cmd(&cmd);
    } else if (!current_cursor[0] && lv_tick_elaps(last_poll) >= 15000) {
        last_poll = lv_tick_get();
        start_cmd(&(q_command_t){.action = Q_FETCH});
    }
}

lv_obj_t *question_card_create(lv_obj_t *chat_screen,
                               question_card_layout_cb_t on_layout,
                               question_card_focus_cb_t on_focus,
                               question_card_status_cb_t on_status)
{
    chat_screen_ref = chat_screen;
    layout_cb = on_layout;
    focus_cb = on_focus;
    status_cb = on_status;

    int w = lv_display_get_horizontal_resolution(NULL);
    card = lv_obj_create(chat_screen);
    eink_style_clean_box(card);
    eink_style_card(card, 22, 2);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, 16, 0);
    lv_obj_set_width(card, w - 32);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

    commands = xQueueCreate(1, sizeof(q_command_t));
    results = xQueueCreate(1, sizeof(q_result_t));
    if (!commands || !results ||
        xTaskCreate(worker, "questions", 12288, NULL, 1, NULL) != pdPASS) {
        if (commands)
            vQueueDelete(commands);
        if (results)
            vQueueDelete(results);
        commands = results = NULL;
        return card;
    }
    last_poll = lv_tick_get() - 15000;
    lv_timer_create(tick, 250, NULL);
    return card;
}

void question_card_on_chat_open(void)
{
    current_cursor[0] = 0;
    fetch_requested = true;
}

bool question_card_visible(void)
{
    return card && has_item && !lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *question_card_widget(void)
{
    return card;
}
