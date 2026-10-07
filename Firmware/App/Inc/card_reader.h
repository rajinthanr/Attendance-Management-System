/**
 * @file    card_reader.h
 * @brief   Level 2 (logic) — polls the reader and reports each card once.
 *
 * A poll turns the field on, waits out the ISO14443 guard time without
 * blocking, activates whatever card is there, and turns the field off again.
 * With the field off between polls a card loses power and resets, so every
 * poll finds it in IDLE and REQA always works.
 *
 * A card held against the reader is seen by poll after poll. It is reported
 * once, when it arrives; it counts as gone only after APP_NFC_REMOVE_MISSES
 * empty polls in a row, so one missed poll does not report it twice.
 *
 * Wake-up mode (use_wakeup): with no card in the field the reader is not
 * polled at all. After an empty poll it is armed instead: the ST25R3916
 * measures the antenna on its own timer and interrupts when the amplitude
 * moves away from a reference taken just after that poll. Arming only ever
 * follows an empty poll, so a card cannot become part of the reference (after
 * a pause, cr_enable() polls once before arming). The interrupt arrives as APP_EVT_NFC_WAKE, which the FSM
 * hands to cr_wake(); polling then runs as above until the field has been
 * empty for APP_NFC_REMOVE_MISSES polls, and the reader is armed again with a
 * fresh reference. A wake-up that finds no card (a hand, a phone, drift)
 * costs those few polls and is counted in false_wakes.
 *
 * The reference is measured with the Measure amplitude command, but the chip
 * compares it with its own wake-up measurement, which can read a few counts
 * apart. Each false wake-up hands back that measurement, and the difference is
 * kept as an offset added to every later reference. False wake-ups that go on
 * after that are noise, and widen the trigger window (delta) step by step.
 */
#ifndef CARD_READER_H
#define CARD_READER_H

#include "app_types.h"
#include "iso14443a.h"

typedef struct {
    bool     enabled;
    bool     use_wakeup;      /**< Arm the wake-up mode while the field is empty. */
    bool     armed;           /**< In wake-up mode, waiting for cr_wake(). */
    bool     woken;           /**< Polling because of a wake-up, no card read yet. */
    bool     field_on;
    uint32_t field_on_ms;     /**< When the field went on for this poll. */
    uint32_t next_poll_ms;

    bool     present;         /**< A reported card is still in the field. */
    uint8_t  present_uid[ISO14443A_UID_MAX];
    uint8_t  present_len;
    uint8_t  misses;          /**< Empty polls in a row (saturates). */

    uint8_t  reference;       /**< Amplitude the last arm compares against. */
    uint8_t  measured;        /**< The Measure amplitude reading behind it. */
    int16_t  offset;          /**< Wake-up reading minus Measure amplitude, learned. */
    uint8_t  delta;           /**< Trigger window, APP_NFC_WAKE_DELTA_MIN..MAX. */
    uint8_t  wake_raw;        /**< The wake-up mode's reading at the last wake-up. */
    uint8_t  false_streak;    /**< False wake-ups in a row. */
    uint8_t  true_streak;     /**< Wake-ups in a row that found a card. */

    iso14443a_status_t last_status;
    uint32_t polls;
    uint32_t errors;          /**< Protocol and reader failures. */
    uint32_t collisions;
    uint32_t wakeups;         /**< Wake-up interrupts acted on. */
    uint32_t false_wakes;     /**< Wake-ups after which no card was read. */
} card_reader_t;

/** @param use_wakeup  Sleep in the reader's wake-up mode between cards. */
void cr_init(card_reader_t *cr, bool use_wakeup);

/**
 * Start or stop the reader. Stopping turns the field off, or leaves wake-up
 * mode, at once; starting polls or arms straight away. Which card is present
 * is remembered across a pause, so a card held through a feedback pattern is
 * not reported again afterwards.
 */
void cr_enable(card_reader_t *cr, bool enable, uint32_t now_ms);

/** True while the field is on, i.e. a poll is in progress. */
bool cr_busy(const card_reader_t *cr);

/** True while the reader waits in wake-up mode (no SPI commands allowed). */
bool cr_armed(const card_reader_t *cr);

/**
 * When cr_task() next has work: the end of the field guard, or the next poll.
 * @return false when it has none until an event (disabled, or armed).
 */
bool cr_next_ms(const card_reader_t *cr, uint32_t *at_ms);

/**
 * The wake-up interrupt fired (APP_EVT_NFC_WAKE): leave wake-up mode and poll
 * now. Ignored when not armed, so a late or repeated event does no harm.
 */
void cr_wake(card_reader_t *cr, uint32_t now_ms);

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
