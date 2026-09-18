/**
 * @file    dedup.c
 * @brief   Level 2 (logic) — most-recently-used duplicate suppression.
 */
#include "dedup.h"
#include "app_config.h"

void dedup_init(dedup_t *d)
{
    uint8_t i;

    for (i = 0u; i < APP_DEDUP_SLOTS; i++) {
        d->id[i] = 0u;
        d->seen[i] = 0u;
    }
    d->next = 0u;
    d->used = 0u;
}

bool dedup_check_and_mark(dedup_t *d, uint32_t id, app_epoch_t now)
{
    uint8_t i;

    for (i = 0u; i < d->used; i++) {
        if (d->id[i] != id) {
            continue;
        }

        /* Unsigned subtraction, so a backwards RTC step (the operator setting
         * the clock) produces a huge delta and simply falls through as "not a
         * duplicate" rather than suppressing a real scan forever. */
        app_epoch_t age = now - d->seen[i];
        bool duplicate = (age < (app_epoch_t)APP_DEDUP_WINDOW_S);

        d->seen[i] = now;
        return duplicate;
    }

    /* Not present: take a free slot, else evict round-robin. Round-robin is
     * enough here because every entry expires after ten seconds anyway. */
    if (d->used < APP_DEDUP_SLOTS) {
        i = d->used++;
    } else {
        i = d->next;
        d->next = (uint8_t)((d->next + 1u) % APP_DEDUP_SLOTS);
    }

    d->id[i] = id;
    d->seen[i] = now;
    return false;
}
