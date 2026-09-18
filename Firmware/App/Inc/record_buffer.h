/**
 * @file    record_buffer.h
 * @brief   Level 2 (logic) — the RAM staging buffer for attendance records.
 *
 * Records accumulate here and are moved to flash in one batch, which is what
 * keeps the average cost per scan low: a flash page erase is milliseconds and
 * tens of milliamps, so doing it once per 102 records rather than once per
 * record is the single biggest energy lever in the design.
 */
#ifndef RECORD_BUFFER_H
#define RECORD_BUFFER_H

#include "app_types.h"
#include "app_config.h"

typedef struct {
    app_record_t item[APP_RAM_RECORDS];
    uint16_t count;
    uint32_t dropped;   /**< Records lost because the buffer was full. */
} record_buffer_t;

void rb_init(record_buffer_t *rb);

/** Append a record. Returns false (and counts a drop) when full. */
bool rb_push(record_buffer_t *rb, const app_record_t *rec);

uint16_t rb_count(const record_buffer_t *rb);
bool rb_is_empty(const record_buffer_t *rb);

/** True once occupancy reaches APP_RAM_FLUSH_PERCENT of capacity. */
bool rb_needs_flush(const record_buffer_t *rb);

/** Read record @p index (0 is oldest). Returns NULL when out of range. */
const app_record_t *rb_peek(const record_buffer_t *rb, uint16_t index);

/** Discard the oldest @p n records after they have been committed to flash. */
void rb_consume(record_buffer_t *rb, uint16_t n);

#endif /* RECORD_BUFFER_H */
