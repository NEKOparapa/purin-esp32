#pragma once

#include <stdbool.h>
#include "lvgl.h"

typedef void (*question_card_layout_cb_t)(void);
typedef void (*question_card_focus_cb_t)(lv_obj_t *textarea);
typedef void (*question_card_status_cb_t)(const char *text);

// Mounts a floating pending-question popup card inside the Agent chat screen.
lv_obj_t *question_card_create(lv_obj_t *chat_screen,
                               question_card_layout_cb_t on_layout,
                               question_card_focus_cb_t on_focus,
                               question_card_status_cb_t on_status);

// Requests an immediate refresh when the Agent chat screen becomes active.
void question_card_on_chat_open(void);

// Returns true when a pending question card is currently shown.
bool question_card_visible(void);

// Returns the popup card widget (NULL before creation).
lv_obj_t *question_card_widget(void);
