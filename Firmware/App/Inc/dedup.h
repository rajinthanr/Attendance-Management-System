/**
 * @file    dedup.h
 * @brief   Level 2 (logic) — "same ID within the last 10 s" suppression.
 *
 * A single last-seen slot would fail the case the flow chart cares about:
 * two people tapping in alternation would each clear the other's slot and both
 * would be recorded twice. This keeps a small most-recently-used table instead.
 */
#ifndef DEDUP_H
#define DEDUP_H

#include "app_types.h"

typedef struct {
    uint32_t id[8];        /**< APP_DEDUP_SLOTS entries; sized by the .c. */
    app_epoch_t seen[8];
    uint8_t  next;         /**< Round-robin eviction cursor. */
    uint8_t  used;
} dedup_t;

void dedup_init(dedup_t *d);

/**
 * Test @p id against the table and record it as seen at @p now.
 *
 * @return true if @p id was already seen inside the window (a duplicate).
 *         The timestamp is refreshed either way, so holding a card against
 *         the reader keeps extending the window rather than logging again.
 */
bool dedup_check_and_mark(dedup_t *d, uint32_t id, app_epoch_t now);

#endif /* DEDUP_H */
