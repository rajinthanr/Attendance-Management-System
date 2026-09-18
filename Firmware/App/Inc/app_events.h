/**
 * @file    app_events.h
 * @brief   Level 2 (logic) — the event queue that decouples ISRs from the FSM.
 *
 * app_event_post() is the single Level 2 symbol that Level 1 is allowed to
 * call. Every interrupt handler in Bsp/ does its acknowledge-and-post and
 * returns; all interpretation happens in the main loop.
 */
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include "app_types.h"

typedef enum {
    APP_EVT_NONE = 0,
    APP_EVT_TOUCH,        /**< Touch IC detected a card approaching. */
    APP_EVT_TIMER,        /**< Short one-shot timer elapsed. */
    APP_EVT_INACTIVITY,   /**< 3-minute inactivity timer elapsed. */
    APP_EVT_USB_ATTACH,   /**< VBUS rising edge. */
    APP_EVT_USB_DETACH,   /**< VBUS falling edge. */
    APP_EVT_USB_ACTIVITY, /**< Host touched the emulated volume. */
    APP_EVT_LOW_BATTERY,  /**< PVD tripped. */
    APP_EVT_BUTTON,       /**< Power button pressed while running. */
    APP_EVT_CAPTURE_FULL  /**< Edge capture buffer filled before the timeout. */
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
