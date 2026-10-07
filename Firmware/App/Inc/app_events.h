/**
 * @file    app_events.h
 * @brief   Level 2 (logic) — the event queue that decouples ISRs from the FSM.
 *
 * app_event_post() is the single Level 2 symbol that Level 1 is allowed to
 * call. Every interrupt handler in Bsp/ does its acknowledge-and-post and
 * returns; all interpretation happens in the main loop.
 *
 * In polling mode most events are posted by the main loop itself, after it
 * has sampled and debounced an input. Moving a source to an interrupt later
 * means posting the same event from its ISR instead.
 */
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include "app_types.h"

typedef enum {
    APP_EVT_NONE = 0,
    APP_EVT_BUTTON_SHORT, /**< Power button tapped. */
    APP_EVT_BUTTON_HOLD,  /**< Power button still held, past APP_BTN_LONG_MS. */
    APP_EVT_BUTTON_LONG,  /**< Released between APP_BTN_LONG_MS and APP_BTN_OFF_MS: new lecture. */
    APP_EVT_BUTTON_OFF,   /**< Power button held past APP_BTN_OFF_MS. */
    APP_EVT_INACTIVITY,   /**< No activity for APP_INACTIVITY_MS. */
    APP_EVT_USB_ATTACH,   /**< VBUS appeared (debounced). */
    APP_EVT_USB_DETACH,   /**< VBUS went away (debounced). */
    APP_EVT_USB_ACTIVITY, /**< Host touched the emulated volume (USB ISR). */
    APP_EVT_LOW_BATTERY   /**< Cell below cutoff on consecutive samples. */
} app_event_t;

/** Reset the queue to empty. Called once during start-up. */
void app_event_init(void);

/**
 * Push an event. Safe from interrupt context. A full queue drops the newest
 * event and increments the overflow counter rather than blocking.
 */
void app_event_post(app_event_t evt);

/**
 * Pop the oldest event, or APP_EVT_NONE if the queue is empty.
 * Only ever called from the main loop.
 */
app_event_t app_event_get(void);

/** True when the queue holds nothing; the FSM uses this to decide to sleep. */
bool app_event_pending(void);

/** Diagnostic: number of events lost to a full queue since boot. */
uint32_t app_event_overflows(void);

#endif /* APP_EVENTS_H */
