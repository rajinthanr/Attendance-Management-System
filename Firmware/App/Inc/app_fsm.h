/**
 * @file    app_fsm.h
 * @brief   Level 2 (logic) — the application state machine.
 *
 * A direct transcription of the flow chart. Every branch in the diagram is a
 * branch here, and every "light sleep mode" terminator is a return to the
 * main loop with an empty event queue.
 */
#ifndef APP_FSM_H
#define APP_FSM_H

#include "app_types.h"
#include "app_events.h"

typedef enum {
    ST_IDLE = 0,   /**< Waiting for a card, in Stop 2 between events. */
    ST_READING,    /**< Carrier on, capturing; Sleep mode only. */
    ST_FEEDBACK,   /**< Stepping an LED/vibration pattern. */
    ST_USB,        /**< Enumerated as mass storage. */
    ST_SHUTDOWN    /**< Finishing up before Standby. */
} app_state_t;

/** Diagnostics, surfaced for test and for a future debug channel. */
typedef struct {
    uint32_t scans_accepted;
    uint32_t scans_duplicate;
    uint32_t scans_unknown;
    uint32_t false_wakes;
    uint32_t records_dropped;
    uint32_t flush_failures;
} app_stats_t;

/** One-time start-up: the flow chart's "Start" through to the first sleep. */
void app_init(void);

/**
 * One pass of the pump: handle the next event, or sleep if there is none.
 *
 * Returns after each pass so the infinite loop stays in main(), where CubeMX
 * puts it. Call it from there, forever, after app_init().
 */
void app_task(void);

/** Feed one event to the machine. Exposed so the host tests can drive it. */
void app_dispatch(app_event_t evt);

app_state_t app_state(void);
const app_stats_t *app_get_stats(void);

#endif /* APP_FSM_H */
