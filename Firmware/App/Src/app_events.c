/**
 * @file    app_events.c
 * @brief   Level 2 (logic) — lock-free-ish single-consumer event queue.
 *
 * One producer set (interrupts) and one consumer (the main loop). The critical
 * section is only needed on the producer side, and only because several
 * different interrupt priorities can post concurrently.
 */
#include "app_events.h"
#include "platform_if.h"

/* Power of two so the wrap is a mask. */
#define EVQ_LEN   16u
#define EVQ_MASK  (EVQ_LEN - 1u)

static volatile app_event_t s_queue[EVQ_LEN];
static volatile uint8_t     s_head;      /* next write slot  */
static volatile uint8_t     s_tail;      /* next read slot   */
static volatile uint32_t    s_overflows;

void app_event_init(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_overflows = 0u;
}

void app_event_post(app_event_t evt)
{
    if (evt == APP_EVT_NONE) {
        return;
    }

    plat_critical_enter();

    uint8_t next = (uint8_t)((s_head + 1u) & EVQ_MASK);
    if (next == s_tail) {
        /* Full. Dropping the newest keeps the oldest (and therefore the
         * causally first) event, which is what the FSM wants. */
        s_overflows++;
    } else {
        s_queue[s_head] = evt;
        s_head = next;
    }

    plat_critical_exit();
}

app_event_t app_event_get(void)
{
    app_event_t evt = APP_EVT_NONE;

    plat_critical_enter();
    if (s_tail != s_head) {
        evt = s_queue[s_tail];
        s_tail = (uint8_t)((s_tail + 1u) & EVQ_MASK);
    }
    plat_critical_exit();

    return evt;
}

bool app_event_pending(void)
{
    return (s_head != s_tail);
}

uint32_t app_event_overflows(void)
{
    return s_overflows;
}
