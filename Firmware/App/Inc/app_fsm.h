/**
 * @file    app_fsm.h
 * @brief   Level 2 (logic) — the application state machine.
 *
 * A polling super-loop: every pass samples the inputs, advances the card
 * reader, the feedback pattern and the timers, handles any events those
 * produced, then sleeps until the next SysTick.
 */
#ifndef APP_FSM_H
#define APP_FSM_H

#include "app_types.h"
#include "app_events.h"

typedef enum {
    ST_IDLE = 0,   /**< Polling for cards. */
    ST_USB,        /**< Enumerated as mass storage; the reader is off. */
    ST_SHUTDOWN    /**< Playing the power-off pattern before Standby. */
} app_state_t;

/** Counters since boot. */
typedef struct {
    uint32_t scans_accepted;
    uint32_t scans_duplicate;
    uint32_t scans_unknown;
    uint32_t scans_rejected_full;
    uint32_t records_dropped;
    uint32_t flush_failures;
    uint32_t nfc_init_failures;
} app_stats_t;

/** One-time start-up: the flow chart's "Start" through to the first poll. */
void app_init(void);

/**
 * One pass of the loop. Returns after sleeping until the next interrupt, so
 * the infinite loop stays in main(), where CubeMX puts it.
 */
void app_task(void);

/** Feed one event to the machine. Exposed so the host tests can drive it. */
void app_dispatch(app_event_t evt);

app_state_t app_state(void);
const app_stats_t *app_get_stats(void);

#endif /* APP_FSM_H */
