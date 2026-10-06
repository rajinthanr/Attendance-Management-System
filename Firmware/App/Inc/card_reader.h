/**
 * @file    card_reader.h
 * @brief   Level 2 (logic) — polls the reader and reports each card once.
 *
 * Every APP_NFC_POLL_MS the field goes on, waits out the ISO14443 guard time
 * without blocking, activates whatever card is there, and goes off again.
 * With the field off between polls a card loses power and resets, so every
 * poll finds it in IDLE and REQA always works.
 *
 * A card held against the reader is seen by poll after poll. It is reported
 * once, when it arrives; it counts as gone only after APP_NFC_REMOVE_MISSES
 * empty polls in a row, so one missed poll does not report it twice.
 */
#ifndef CARD_READER_H
#define CARD_READER_H

#include "app_types.h"
#include "iso14443a.h"

typedef struct {
    bool     enabled;
    bool     field_on;
    uint32_t field_on_ms;     /**< When the field went on for this poll. */
    uint32_t next_poll_ms;

    bool     present;         /**< A reported card is still in the field. */
    uint8_t  present_uid[ISO14443A_UID_MAX];
    uint8_t  present_len;
    uint8_t  misses;

    iso14443a_status_t last_status;
    uint32_t polls;
    uint32_t errors;          /**< Protocol and reader failures. */
    uint32_t collisions;
} card_reader_t;

void cr_init(card_reader_t *cr);

/**
 * Start or stop polling. Stopping turns the field off at once; starting polls
 * straight away. Which card is present is remembered across a pause, so a
 * card held through a feedback pattern is not reported again afterwards.
 */
void cr_enable(card_reader_t *cr, bool enable, uint32_t now_ms);

/** True while the field is on, i.e. a poll is in progress. */
bool cr_busy(const card_reader_t *cr);

/**
 * Advance polling. Call every pass of the main loop.
 * @return true when a newly presented card has been read into @p card.
 */
bool cr_task(card_reader_t *cr, uint32_t now_ms, iso14443a_card_t *card);

/**
 * The 32-bit ID stored in the log and the CSV.
 *
 * A 4-byte UID maps straight across, first byte most significant, so
 * 0A F4 1A 9E becomes 0x0AF41A9E, which the CSV prints as 0183769758.
 * Longer UIDs keep their last four bytes: the first byte is the manufacturer
 * code and is the same on every card from one vendor.
 */
uint32_t card_id_from_uid(const uint8_t *uid, uint8_t len);

#endif /* CARD_READER_H */
