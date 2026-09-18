/**
 * @file    record_buffer.c
 * @brief   Level 2 (logic) — RAM staging buffer.
 *
 * A plain array rather than a ring: the buffer is always drained from the
 * front and completely, and a linear layout means log_store can hand a
 * contiguous span to the flash writer.
 */
#include "record_buffer.h"

/* Occupancy at which the flow chart's "RAM buffer >= 80 % full?" turns true. */
#define FLUSH_LEVEL  ((uint16_t)(((uint32_t)APP_RAM_RECORDS * APP_RAM_FLUSH_PERCENT) / 100u))

void rb_init(record_buffer_t *rb)
{
    rb->count = 0u;
    rb->dropped = 0u;
}

bool rb_push(record_buffer_t *rb, const app_record_t *rec)
{
    if (rb->count >= APP_RAM_RECORDS) {
        rb->dropped++;
        return false;
    }
    rb->item[rb->count] = *rec;
    rb->count++;
    return true;
}

uint16_t rb_count(const record_buffer_t *rb)
{
    return rb->count;
}

bool rb_is_empty(const record_buffer_t *rb)
{
    return (rb->count == 0u);
}

bool rb_needs_flush(const record_buffer_t *rb)
{
    return (rb->count >= FLUSH_LEVEL);
}

const app_record_t *rb_peek(const record_buffer_t *rb, uint16_t index)
{
    if (index >= rb->count) {
        return NULL;
    }
    return &rb->item[index];
}

void rb_consume(record_buffer_t *rb, uint16_t n)
{
    uint16_t i;

    if (n >= rb->count) {
        rb->count = 0u;
        return;
    }

    /* Partial commit (the flash area filled mid-flush): keep the tail. */
    for (i = 0u; i < (uint16_t)(rb->count - n); i++) {
        rb->item[i] = rb->item[i + n];
    }
    rb->count = (uint16_t)(rb->count - n);
}
