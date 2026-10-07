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
    b->hold_sent = false;
    b->off_sent = false;
    b->raw_changed_ms = now_ms;
    b->down_ms = now_ms;
    b->tapped = false;
    b->tap_ms = now_ms;
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
            b->hold_sent = false;
            b->off_sent = false;
        } else {
            bool ignored = b->locked || b->off_sent;

            b->locked = false;
            if (!ignored) {
                if (b->hold_sent) {
                    b->tapped = false;
                    return BTN_LONG;
                }
                if (b->tapped && (uint32_t)(now_ms - b->tap_ms) <= APP_BTN_DOUBLE_MS) {
                    b->tapped = false;
                    return BTN_DOUBLE;
                }
                b->tapped = true;
                b->tap_ms = now_ms;
                return BTN_SHORT;
            }
        }
    }

    if (b->down && !b->locked) {
        uint32_t held = (uint32_t)(now_ms - b->down_ms);

        if (!b->hold_sent && held >= APP_BTN_LONG_MS) {
            b->hold_sent = true;
            return BTN_HOLD;
        }
        if (!b->off_sent && held >= APP_BTN_OFF_MS) {
            b->off_sent = true;
            return BTN_OFF;
        }
    }

    return BTN_NONE;
}

bool btn_is_down(const button_t *b)
{
    return b->down;
}

bool btn_next_ms(const button_t *b, uint32_t *at_ms)
{
    if (b->raw != b->down) {
        *at_ms = b->raw_changed_ms + APP_BTN_DEBOUNCE_MS;
        return true;
    }
    if (b->down && !b->locked) {
        if (!b->hold_sent) {
            *at_ms = b->down_ms + APP_BTN_LONG_MS;
            return true;
        }
        if (!b->off_sent) {
            *at_ms = b->down_ms + APP_BTN_OFF_MS;
            return true;
        }
    }
    return false;
}
