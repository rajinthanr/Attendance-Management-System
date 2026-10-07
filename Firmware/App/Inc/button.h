/**
 * @file    button.h
 * @brief   Level 2 (logic) — debounce the power button and classify presses.
 *
 * Fed the raw pin level every pass of the main loop. A press is one of:
 *
 *   released before APP_BTN_LONG_MS            BTN_SHORT on release, or
 *     ...within APP_BTN_DOUBLE_MS of the       BTN_DOUBLE instead (the first
 *        previous short press's release        tap still gave its BTN_SHORT)
 *   held past APP_BTN_LONG_MS                  BTN_HOLD at that moment, then
 *     ...and released before APP_BTN_OFF_MS    BTN_LONG on release
 *     ...or held on past APP_BTN_OFF_MS        BTN_OFF at that moment; the
 *                                              release then reports nothing
 *
 * The press that woke the unit from Standby is still held when this starts,
 * so a button found down at start-up is ignored until it is released.
 */
#ifndef BUTTON_H
#define BUTTON_H

#include "app_types.h"

typedef enum {
    BTN_NONE = 0,
    BTN_SHORT,      /**< Tapped. */
    BTN_HOLD,       /**< Still held, just crossed APP_BTN_LONG_MS. */
    BTN_LONG,       /**< Released between APP_BTN_LONG_MS and APP_BTN_OFF_MS. */
    BTN_OFF,        /**< Still held, just crossed APP_BTN_OFF_MS. */
    BTN_DOUBLE      /**< A second tap soon after a first one. */
} button_event_t;

typedef struct {
    bool     raw;
    bool     down;           /**< Debounced level. */
    bool     locked;         /**< Ignore this press; wait for a release. */
    bool     hold_sent;
    bool     off_sent;
    uint32_t raw_changed_ms;
    uint32_t down_ms;
    bool     tapped;         /**< A BTN_SHORT that a second tap may pair with. */
    uint32_t tap_ms;         /**< When that tap was released. */
} button_t;

void btn_init(button_t *b, bool pressed_now, uint32_t now_ms);

button_event_t btn_update(button_t *b, bool raw_pressed, uint32_t now_ms);

/** Debounced level, including a locked press. */
bool btn_is_down(const button_t *b);

/**
 * When btn_update() could next have something to say without the raw level
 * changing (a change wakes the loop by itself): the end of a debounce, or the
 * hold and off thresholds of a press.
 * @return false when nothing is pending.
 */
bool btn_next_ms(const button_t *b, uint32_t *at_ms);

#endif /* BUTTON_H */
