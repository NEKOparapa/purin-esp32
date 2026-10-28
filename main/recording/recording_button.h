#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    bool fired;
    uint64_t changed_ms;
    uint64_t pressed_ms;
} recording_button_t;

// 单调毫秒时钟；首次启动必须先稳定松手，避免把开机残留按住当作录音指令。
static inline bool recording_button_update(recording_button_t *button, bool pressed, uint64_t now_ms)
{
    if (!button->initialized) {
        *button = (recording_button_t) {
            .initialized = true, .raw_pressed = pressed,
            .stable_pressed = true, .changed_ms = now_ms,
        };
    }
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->changed_ms = now_ms;
    }
    if (button->stable_pressed != button->raw_pressed && now_ms - button->changed_ms >= 30) {
        button->stable_pressed = button->raw_pressed;
        if (button->stable_pressed) {
            button->pressed_ms = now_ms;
            button->fired = false;
        } else {
            button->armed = true;
            button->fired = false;
        }
    }
    if (button->armed && button->stable_pressed && button->raw_pressed && !button->fired &&
        now_ms - button->pressed_ms >= 2000) {
        button->fired = true;
        return true;
    }
    return false;
}
