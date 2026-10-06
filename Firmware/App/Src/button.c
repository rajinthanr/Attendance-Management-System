/**
 * @file    button.c
 * @brief   Level 2 (logic) — button debouncing.
 */
#include "button.h"
#include "app_config.h"

void btn_init(button_t *b, bool pressed_now, uint32_t now_ms)
{
    b->raw = pressed_now;
    b->down = pressed_now;
    b->locked = pressed_now;
    b->long_sent = false;
    b->raw_changed_ms = now_ms;
    b->down_ms = now_ms;
}

button_event_t btn_update(button_t *b, bool raw_pressed, uint32_t now_ms)
{
    if (raw_pressed != b->raw) {
        b->raw = raw_pressed;
        b->raw_changed_ms = now_ms;
    }

    if (b->raw != b->down &&
        (uint32_t)(now_ms - b->raw_changed_ms) >= APP_BTN_DEBOUNCE_MS) {
        b->down = b->raw;

        if (b->down) {
            b->down_ms = now_ms;
            b->long_sent = false;
        } else {
            bool ignored = b->locked || b->long_sent;

            b->locked = false;
            if (!ignored) {
                return BTN_SHORT;
            }
        }
    }

    if (b->down && !b->locked && !b->long_sent &&
        (uint32_t)(now_ms - b->down_ms) >= APP_BTN_LONG_MS) {
        b->long_sent = true;
        return BTN_LONG;
    }

    return BTN_NONE;
}

bool btn_is_down(const button_t *b)
{
    return b->down;
}
