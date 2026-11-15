#include "agent_screen.h"
#include "agent_client.h"
#include "recorder.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

extern const uint8_t chat_sc_start[] asm("_binary_chat_sc_ttf_start");
extern const uint8_t chat_sc_end[] asm("_binary_chat_sc_ttf_end");

static lv_obj_t *home, *screen, *thread, *draft, *send_button, *keyboard, *candidates, *status, *read_button;
static const lv_font_t *chat_font;
static lv_obj_t *ime, *language;
static bool busy, poll_job, use_pinyin;
static unsigned pinyin_letters;
static agent_action_t active_action;
static uint32_t last_poll;
static char reading[AGENT_ID_BYTES];
enum { HISTORY_ROWS = 16 };
static struct {
    char id[AGENT_ID_BYTES];
    lv_obj_t *widget;
} rows[HISTORY_ROWS];
static size_t row_count;

static void set_status(const char *text)
{
    if (strcmp(lv_label_get_text(status), text)) lv_label_set_text(status, text);
}

static bool start(agent_action_t action, const char *value)
{
    if (busy) return false;
    if (!agent_client_start(action, value)) {
        set_status("Device client unavailable");
        return false;
    }
    busy = true;
    active_action = action;
    switch (action) {
    case AGENT_SEND: set_status("Sending message"); break;
    case AGENT_CAPTURE: set_status("Sharing recorder state"); break;
    case AGENT_READ: set_status("Marking reply as read"); break;
    case AGENT_INBOX: set_status("Checking inbox"); break;
    case AGENT_SYNC: if (!poll_job) set_status("Checking last message"); break;
    }
    return true;
}

static void reader_layout(bool editing)
{
    int h = lv_display_get_vertical_resolution(NULL);
    lv_obj_set_height(thread, h - (editing ? 460 : 245));
    int draft_y = h - (editing ? 391 : 175);
    lv_obj_set_y(draft, draft_y);
    lv_obj_set_y(send_button, draft_y);
    if (editing) {
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (candidates) lv_obj_add_flag(candidates, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(draft, LV_STATE_FOCUSED);
    }
}

static lv_obj_t *add_row(const char *id, const char *speaker, const char *text)
{
    for (size_t i = 0; i < row_count; ++i) {
        if (!strcmp(rows[i].id, id)) return rows[i].widget;
    }
    if (row_count == HISTORY_ROWS) {
        lv_obj_delete(rows[0].widget);
        memmove(rows, rows + 1, sizeof(rows) - sizeof(rows[0]));
        --row_count;
    }
    lv_obj_t *label = lv_label_create(thread);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(label, chat_font, 0);
    lv_label_set_text_fmt(label, "%s\n%s", speaker, text);
    strlcpy(rows[row_count].id, id, sizeof(rows[row_count].id));
    rows[row_count++].widget = label;
    lv_obj_update_layout(thread);
    return label;
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    agent_result_t *result = agent_client_take_result();
    if (result) {
        busy = false;
        if (active_action == AGENT_SEND || active_action == AGENT_SYNC) {
            poll_job = result->poll_job;
            last_poll = lv_tick_get();
        }
        if (result->sent && !strcmp(lv_textarea_get_text(draft), result->question)) {
            lv_textarea_set_text(draft, "");
        }
        if (result->message_id[0] && result->sent) {
            lv_obj_scroll_to_view(add_row(result->message_id, "You", result->question), LV_ANIM_OFF);
        } else if (result->message_id[0]) {
            add_row(result->message_id, "You", result->question);
        }
        if (result->acknowledged) reading[0] = 0;
        for (agent_reply_t *r = result->replies; r; r = r->next) {
            if (!r->text) continue;
            lv_obj_t *row = add_row(r->id, r->notification ? "Notification" : "Agent", r->text);
            if (r->unread || active_action == AGENT_SYNC) {
                lv_obj_update_layout(thread);
                // Start even a multi-page answer at its first line.
                int32_t top = lv_obj_get_y(row);
                lv_obj_scroll_to_y(thread, top, LV_ANIM_OFF);
            }
            if (r->unread) {
                strlcpy(reading, r->id, sizeof(reading));
            }
        }
        if (reading[0]) lv_obj_remove_flag(read_button, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(read_button, LV_OBJ_FLAG_HIDDEN);
        set_status(result->status);
        agent_result_free(result);
    }
    if (!busy && poll_job && lv_screen_active() == screen && lv_tick_elaps(last_poll) >= 5000) {
        start(AGENT_SYNC, NULL);
    }
}

void agent_screen_hide(void)
{
    if (screen && lv_screen_active() == screen) lv_screen_load(home);
}

static void create_keyboard(void);

static void action(lv_event_t *event)
{
    switch ((char)(intptr_t)lv_event_get_user_data(event)) {
    case 'b': agent_screen_hide(); break;
    case 'l':
        if (ime) lv_obj_delete(ime); // LVGL's IME also owns its keyboard and candidate panel.
        else lv_obj_delete(keyboard);
        use_pinyin = !use_pinyin;
        lv_label_set_text(language, use_pinyin ? "拼音" : "EN");
        lv_obj_center(language);
        create_keyboard();
        reader_layout(true);
        break;
    case 'e': reader_layout(true); break;
    case 'd': reader_layout(false); break;
    case 'r': start(AGENT_SYNC, NULL); break;
    case 'i': start(AGENT_INBOX, NULL); break;
    case 'a': if (reading[0]) start(AGENT_READ, reading); break;
    case 'c': {
        const char *states[] = {"idle", "starting", "recording", "saving", "error"};
        recorder_status_t snapshot = recorder_get_status();
        start(AGENT_CAPTURE, states[snapshot.state]);
        break;
    }
    case 's': {
        const char *text = lv_textarea_get_text(draft);
        if (!text[strspn(text, " \n\t\r")]) {
            set_status("Enter a message first");
        } else if (strlen(text) >= AGENT_INPUT_BYTES) {
            set_status("Shorten this message");
        } else if (start(AGENT_SEND, text)) {
            reader_layout(false);
        }
        break;
    }
    }
}

static lv_obj_t *button(int x, int y, int w, const char *text, char command)
{
    lv_obj_t *obj = lv_button_create(screen);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, 50);
    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(obj, action, LV_EVENT_CLICKED, (void *)(intptr_t)command);
    return obj;
}

// The LVGL IME has a fixed spelling buffer. In Pinyin mode each syllable is
// committed via a candidate. Latin free typing uses the plain keyboard.
static void spelling_key(lv_event_t *event)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(keyboard);
    const char *key = lv_buttonmatrix_get_button_text(keyboard, id);
    if (!key) return;
    if (key[0] >= 'a' && key[0] <= 'z' && !key[1]) {
        if (pinyin_letters >= 7) {
            // The keyboard's own handler has already inserted this letter.
            lv_textarea_delete_char(draft);
            lv_event_stop_processing(event);
        } else ++pinyin_letters;
    } else if (!strcmp(key, LV_SYMBOL_BACKSPACE)) {
        if (pinyin_letters) --pinyin_letters;
    } else if (!strcmp(key, "ABC") || !strcmp(key, "abc") || !strcmp(key, "1#") ||
               !strcmp(key, "123") || !strcmp(key, LV_SYMBOL_OK) || !strcmp(key, LV_SYMBOL_NEW_LINE)) {
        pinyin_letters = 0;
    }
}

static void candidate_key(lv_event_t *event)
{
    (void)event;
    uint32_t index = lv_buttonmatrix_get_selected_button(candidates);
    if (index >= 1 && index <= LV_IME_PINYIN_CAND_TEXT_NUM) pinyin_letters = 0;
}

static void create_keyboard(void)
{
    int w = lv_display_get_horizontal_resolution(NULL);
    ime = candidates = NULL;
    pinyin_letters = 0;
    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, w, 264);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, draft);
    lv_obj_add_event_cb(keyboard, action, LV_EVENT_READY, (void *)(intptr_t)'d');
    lv_obj_add_event_cb(keyboard, action, LV_EVENT_CANCEL, (void *)(intptr_t)'d');
    if (use_pinyin) {
        ime = lv_ime_pinyin_create(screen);
        lv_obj_set_style_text_font(ime, chat_font, 0);
        lv_obj_add_event_cb(keyboard, spelling_key, LV_EVENT_VALUE_CHANGED, NULL);
        lv_ime_pinyin_set_keyboard(ime, keyboard);
        candidates = lv_ime_pinyin_get_cand_panel(ime);
        lv_obj_set_size(candidates, w, 52);
        lv_obj_align_to(candidates, keyboard, LV_ALIGN_OUT_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(candidates, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(candidates, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(candidates, candidate_key, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

const lv_font_t *agent_text_font(void)
{
    if (!chat_font) {
        lv_font_t *font = lv_tiny_ttf_create_data(chat_sc_start, chat_sc_end - chat_sc_start, 22);
        if (font) font->fallback = LV_FONT_DEFAULT;
        chat_font = font ? font : LV_FONT_DEFAULT;
    }
    return chat_font;
}

static void create_screen(void)
{
    home = lv_screen_active();
    screen = lv_obj_create(NULL);
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    agent_text_font();

    button(8, 8, 95, "Home", 'b');
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Agent");
    lv_obj_set_pos(title, 118, 18);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    language = lv_obj_get_child(button(w - 285, 8, 83, "EN", 'l'), 0);
    lv_obj_set_style_text_font(language, chat_font, 0);
    button(w - 195, 8, 87, "Retry", 'r');
    button(w - 102, 8, 94, "Inbox", 'i');

    thread = lv_obj_create(screen);
    lv_obj_set_pos(thread, 8, 62);
    lv_obj_set_width(thread, w - 16);
    lv_obj_set_flex_flow(thread, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(thread, LV_DIR_VER);
    lv_obj_set_style_pad_all(thread, 10, 0);
    lv_obj_set_style_pad_row(thread, 14, 0);
    draft = lv_textarea_create(screen);
    lv_obj_set_pos(draft, 8, 0);
    lv_obj_set_size(draft, w - 116, 60);
    lv_textarea_set_one_line(draft, true);
    lv_textarea_set_max_length(draft, 192);
    lv_textarea_set_placeholder_text(draft, "Write a message...");
    lv_obj_set_style_text_font(draft, chat_font, 0);
    lv_obj_set_style_anim_duration(draft, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(draft, action, LV_EVENT_CLICKED, (void *)(intptr_t)'e');
    send_button = button(w - 100, 0, 92, "Send", 's');

    button(8, h - 108, 168, "Share state", 'c');
    read_button = button(182, h - 108, 160, "Read / next", 'a');
    lv_obj_add_flag(read_button, LV_OBJ_FLAG_HIDDEN);
    status = lv_label_create(screen);
    lv_obj_set_pos(status, 12, h - 50);
    lv_obj_set_width(status, w - 24);
    lv_label_set_text(status, "");

    create_keyboard();
    reader_layout(false);
    lv_timer_create(tick, 250, NULL);
}

void agent_screen_open(void)
{
    if (!screen) create_screen();
    lv_screen_load(screen);
    if (!busy) start(AGENT_SYNC, NULL);
}
