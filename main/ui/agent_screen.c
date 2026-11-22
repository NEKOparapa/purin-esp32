#include "agent_screen.h"
#include "agent_client.h"
#include "eink_ui.h"
#include "home_graphics.h"
#include "lvgl.h"
#include "question_card.h"
#include <stdio.h>
#include <string.h>

extern const uint8_t chat_sc_start[] asm("_binary_chat_sc_ttf_start");
extern const uint8_t chat_sc_end[] asm("_binary_chat_sc_ttf_end");

static lv_obj_t *home, *screen, *thread, *composer_row;
static lv_obj_t *draft, *send_button, *keyboard, *candidates;
static lv_obj_t *active_textarea;
static const lv_font_t *chat_font;
static lv_obj_t *ime, *language, *refresh_label;
static bool busy, poll_job, use_pinyin, is_editing;
static unsigned pinyin_letters;
static agent_action_t active_action;
static uint32_t last_poll;
enum { HISTORY_ROWS = 16, COMPOSER_HEIGHT = 56, COMPOSER_CONTROL_HEIGHT = 54 };
static struct {
    char id[AGENT_ID_BYTES];
    lv_obj_t *widget;
} rows[HISTORY_ROWS];
static size_t row_count;

static void set_refresh_caption(const char *text)
{
    if (refresh_label && text && strcmp(lv_label_get_text(refresh_label), text) != 0) {
        lv_label_set_text(refresh_label, text);
    }
}

static bool start(agent_action_t action, const char *value)
{
    if (busy) return false;
    if (!agent_client_start(action, value)) return false;
    busy = true;
    active_action = action;
    if (action == AGENT_SEND) {
        set_refresh_caption("发送中");
    } else if (!poll_job) {
        set_refresh_caption("同步中");
    }
    return true;
}

static void reader_layout(bool editing)
{
    is_editing = editing;
    int h = lv_display_get_vertical_resolution(NULL);
    const int kb_h = 264;
    const int cand_h = 50;
    lv_obj_t *qcard = question_card_widget();
    const bool has_q = question_card_visible();

    if (editing) {
        int bottom_anchor = h - kb_h - (use_pinyin ? cand_h : 0);
        const bool editing_question = has_q && active_textarea && active_textarea != draft;
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (candidates) lv_obj_remove_flag(candidates, LV_OBJ_FLAG_HIDDEN);

        if (editing_question && qcard) {
            lv_obj_add_flag(composer_row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(qcard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(qcard);
            int card_h = lv_obj_get_height(qcard);
            int card_y = bottom_anchor - card_h - 6;
            if (card_y < EINK_TOP_BAR_H + 8) card_y = EINK_TOP_BAR_H + 8;
            lv_obj_set_y(qcard, card_y);
            int thread_h = card_y - EINK_TOP_BAR_H - 6;
            lv_obj_set_height(thread, thread_h > 40 ? thread_h : 40);
        } else {
            if (qcard) lv_obj_add_flag(qcard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(composer_row, LV_OBJ_FLAG_HIDDEN);
            int composer_y = bottom_anchor - COMPOSER_HEIGHT - 6;
            lv_obj_set_y(composer_row, composer_y);
            lv_obj_set_height(thread, composer_y - EINK_TOP_BAR_H - 6);
        }
    } else {
        active_textarea = draft;
        int composer_y = h - COMPOSER_HEIGHT - 16;
        lv_obj_remove_flag(composer_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(composer_row, composer_y);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (candidates) lv_obj_add_flag(candidates, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(draft, LV_STATE_FOCUSED);

        if (has_q && qcard) {
            lv_obj_remove_flag(qcard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(qcard);
            int card_h = lv_obj_get_height(qcard);
            int card_y = composer_y - card_h - 10;
            if (card_y < EINK_TOP_BAR_H + 64) card_y = EINK_TOP_BAR_H + 64;
            lv_obj_set_y(qcard, card_y);
            int thread_h = card_y - EINK_TOP_BAR_H - 6;
            lv_obj_set_height(thread, thread_h > 48 ? thread_h : 48);
        } else {
            if (qcard) lv_obj_add_flag(qcard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(thread, composer_y - EINK_TOP_BAR_H - 6);
        }
    }
}

static void on_question_layout(void)
{
    if (!screen) return;
    if (!question_card_visible() && active_textarea != draft) {
        active_textarea = draft;
        if (keyboard) lv_keyboard_set_textarea(keyboard, draft);
        reader_layout(false);
        return;
    }
    reader_layout(is_editing);
}

static void on_question_focus_input(lv_obj_t *ta)
{
    if (!ta || !keyboard) return;
    active_textarea = ta;
    lv_keyboard_set_textarea(keyboard, ta);
    reader_layout(true);
}

static lv_obj_t *add_row(const char *id, bool is_user, const char *text)
{
    for (size_t i = 0; i < row_count; ++i) {
        if (!strcmp(rows[i].id, id)) return rows[i].widget;
    }
    if (row_count == HISTORY_ROWS) {
        lv_obj_delete(rows[0].widget);
        memmove(rows, rows + 1, sizeof(rows) - sizeof(rows[0]));
        --row_count;
    }

    lv_obj_t *row = lv_obj_create(thread);
    eink_style_clean_box(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *bubble = lv_obj_create(row);
    eink_style_clean_box(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, LV_PCT(is_user ? 84 : 90), LV_PART_MAIN);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 6, LV_PART_MAIN);

    if (is_user) {
        lv_obj_set_style_bg_color(bubble, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_color(bubble, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(bubble, 2, LV_PART_MAIN);
    }

    lv_obj_t *label = lv_label_create(bubble);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, is_user ? 336 : 364, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(label, chat_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, is_user ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 5, LV_PART_MAIN);
    lv_label_set_text(label, text);

    strlcpy(rows[row_count].id, id, sizeof(rows[row_count].id));
    rows[row_count++].widget = row;
    lv_obj_update_layout(thread);
    return row;
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    agent_result_t *result = agent_client_take_result();
    if (result) {
        busy = false;
        set_refresh_caption("刷新");
        if (active_action == AGENT_SEND || active_action == AGENT_SYNC) {
            poll_job = result->poll_job;
            last_poll = lv_tick_get();
        }
        if (result->sent && !strcmp(lv_textarea_get_text(draft), result->question)) {
            lv_textarea_set_text(draft, "");
        }
        if (result->message_id[0] && result->sent) {
            lv_obj_scroll_to_view(add_row(result->message_id, true, result->question), LV_ANIM_OFF);
        } else if (result->message_id[0]) {
            add_row(result->message_id, true, result->question);
        }
        for (agent_reply_t *r = result->replies; r; r = r->next) {
            if (!r->text) continue;
            lv_obj_t *row = add_row(r->id, false, r->text);
            if (active_action == AGENT_SYNC || result->acknowledged) {
                lv_obj_update_layout(thread);
                // Start even a multi-page answer at its first line.
                int32_t top = lv_obj_get_y(row);
                lv_obj_scroll_to_y(thread, top, LV_ANIM_OFF);
            }
        }
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
    case 'e':
        active_textarea = draft;
        if (keyboard) lv_keyboard_set_textarea(keyboard, draft);
        reader_layout(true);
        break;
    case 'd': reader_layout(false); break;
    case 'r':
        question_card_on_chat_open();
        start(AGENT_SYNC, NULL);
        break;
    case 's': {
        const char *text = lv_textarea_get_text(draft);
        if (!text[strspn(text, " \n\t\r")] || strlen(text) >= AGENT_INPUT_BYTES) {
            break;
        }
        if (start(AGENT_SEND, text)) {
            reader_layout(false);
        }
        break;
    }
    }
}

static void on_back(lv_event_t *event)
{
    (void)event;
    agent_screen_hide();
}

// The LVGL IME has a fixed spelling buffer. In Pinyin mode each syllable is
// committed via a candidate. Latin free typing uses the plain keyboard.
static void spelling_key(lv_event_t *event)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(keyboard);
    const char *key = lv_buttonmatrix_get_button_text(keyboard, id);
    if (!key) return;
    lv_obj_t *target = active_textarea ? active_textarea : draft;
    if (key[0] >= 'a' && key[0] <= 'z' && !key[1]) {
        if (pinyin_letters >= 7) {
            // The keyboard's own handler has already inserted this letter.
            lv_textarea_delete_char(target);
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
    eink_style_keyboard(keyboard);
    lv_obj_set_size(keyboard, w, 264);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, active_textarea ? active_textarea : draft);
    lv_obj_add_event_cb(keyboard, action, LV_EVENT_READY, (void *)(intptr_t)'d');
    lv_obj_add_event_cb(keyboard, action, LV_EVENT_CANCEL, (void *)(intptr_t)'d');
    if (use_pinyin) {
        ime = lv_ime_pinyin_create(screen);
        lv_obj_set_style_text_font(ime, chat_font, 0);
        lv_obj_add_event_cb(keyboard, spelling_key, LV_EVENT_VALUE_CHANGED, NULL);
        lv_ime_pinyin_set_keyboard(ime, keyboard);
        candidates = lv_ime_pinyin_get_cand_panel(ime);
        eink_style_candidates(candidates, chat_font);
        lv_obj_set_size(candidates, w, 50);
        lv_obj_align_to(candidates, keyboard, LV_ALIGN_OUT_TOP_MID, 0, 0);
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
    eink_style_screen(screen);
    lv_obj_set_size(screen, w, h);
    agent_text_font();

    // Minimal Top Navigation Bar: Back + 对话 + [EN/拼音] + [刷新]
    lv_obj_t *right_slot = NULL;
    eink_top_bar_font(screen, "对话", eink_heading_font(), on_back, &right_slot);
    lv_obj_t *lang_btn = eink_pill_button(right_slot, "EN", eink_small_font(), 40, false, action,
                                          (void *)(intptr_t)'l');
    lv_obj_set_style_pad_hor(lang_btn, 14, LV_PART_MAIN);
    language = lv_obj_get_child(lang_btn, 0);

    lv_obj_t *refresh_btn = eink_pill_button(right_slot, "刷新", eink_small_font(), 40, false,
                                             action, (void *)(intptr_t)'r');
    lv_obj_set_style_pad_hor(refresh_btn, 14, LV_PART_MAIN);
    refresh_label = lv_obj_get_child(refresh_btn, 0);

    // Chat Message Thread (pure white paper, spacious padding)
    thread = lv_obj_create(screen);
    eink_style_clean_box(thread);
    lv_obj_set_pos(thread, 0, EINK_TOP_BAR_H);
    lv_obj_set_width(thread, w);
    lv_obj_set_flex_flow(thread, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(thread, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(thread, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(thread, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(thread, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(thread, 12, LV_PART_MAIN);

    // Inline Pending Question Popup Card (shown dynamically above composer)
    question_card_create(screen, on_question_layout, on_question_focus_input, NULL);

    // Floating Capsule Composer Row (pure bottom edge, zero status bar below)
    composer_row = lv_obj_create(screen);
    eink_style_clean_box(composer_row);
    lv_obj_remove_flag(composer_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(composer_row, 16, 0);
    lv_obj_set_size(composer_row, w - 32, COMPOSER_HEIGHT);
    lv_obj_set_flex_flow(composer_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(composer_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(composer_row, 10, LV_PART_MAIN);

    lv_obj_t *input_frame = eink_textarea(composer_row, chat_font, COMPOSER_CONTROL_HEIGHT,
                                         LV_RADIUS_CIRCLE, &draft);
    lv_obj_set_flex_grow(input_frame, 1);
    active_textarea = draft;
    lv_textarea_set_max_length(draft, 192);
    lv_textarea_set_placeholder_text(draft, "发消息给 Wintermute...");
    lv_obj_add_event_cb(draft, action, LV_EVENT_CLICKED, (void *)(intptr_t)'e');

    send_button = eink_pill_button(composer_row, "发送", &home_controls, COMPOSER_CONTROL_HEIGHT,
                                  true, action, (void *)(intptr_t)'s');
    lv_obj_set_width(send_button, 84);

    create_keyboard();
    reader_layout(false);
    lv_timer_create(tick, 250, NULL);
}

void agent_screen_open(void)
{
    if (!screen) create_screen();
    lv_screen_load(screen);
    question_card_on_chat_open();
    if (!busy) start(AGENT_SYNC, NULL);
}
