/**
 * @file    button.h
 * @brief   Level 2 (logic) — debounce the power button and classify presses.
 *
 * Fed the raw pin level every pass of the main loop. A press shorter than
 * APP_BTN_LONG_MS is reported on release; a long press is reported once,
 * while still held, the moment it crosses the threshold.
 *
 * The press that woke the unit from Standby is still held when this starts,
 * so a button found down at start-up is ignored until it is released.
 */
#ifndef BUTTON_H
#define BUTTON_H

#include "app_types.h"

typedef enum {
    BTN_NONE = 0,
    BTN_SHORT,
    BTN_LONG
} button_event_t;

typedef struct {
    bool     raw;
    bool     down;           /**< Debounced level. */
    bool     locked;         /**< Ignore this press; wait for a release. */
    bool     long_sent;
    uint32_t raw_changed_ms;
    uint32_t down_ms;
} button_t;

void btn_init(button_t *b, bool pressed_now, uint32_t now_ms);

button_event_t btn_update(button_t *b, bool raw_pressed, uint32_t now_ms);

/** Debounced level, including a locked press. */
bool btn_is_down(const button_t *b);

#endif /* BUTTON_H */
