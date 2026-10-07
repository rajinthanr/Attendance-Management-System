/**
 * @file    card_reader.c
 * @brief   Level 2 (logic) — reader polling, wake-up arming and card
 *          presence tracking.
 */
#include "card_reader.h"
#include "app_config.h"
#include "platform_if.h"

#define MISSES_MAX 0xFFu

void cr_init(card_reader_t *cr, bool use_wakeup)
{
    uint8_t i;

    cr->enabled = false;
    cr->use_wakeup = use_wakeup;
    cr->armed = false;
    cr->woken = false;
    cr->field_on = false;
    cr->field_on_ms = 0u;
    cr->next_poll_ms = 0u;
    cr->present = false;
    for (i = 0u; i < ISO14443A_UID_MAX; i++) {
        cr->present_uid[i] = 0u;
    }
    cr->present_len = 0u;
    /* Nothing has been seen yet: the first empty poll arms. */
    cr->misses = APP_NFC_REMOVE_MISSES;
    cr->reference = 0u;
    cr->measured = 0u;
    cr->offset = 0;
    cr->delta = APP_NFC_WAKE_DELTA_MIN;
    cr->wake_raw = 0u;
    cr->false_streak = 0u;
    cr->true_streak = 0u;
    cr->last_status = ISO14443A_NO_CARD;
    cr->polls = 0u;
    cr->errors = 0u;
    cr->collisions = 0u;
    cr->wakeups = 0u;
    cr->false_wakes = 0u;
}

static void disarm(card_reader_t *cr)
{
    cr->armed = false;
    if (!plat_nfc_wakeup_disarm(&cr->wake_raw)) {
        cr->errors++;
    }
}

/**
 * The last wake-up found nothing. The chip's own reading at that moment, with
 * the field empty, says where its wake-up measurement sits relative to Measure
 * amplitude: keep that as the offset. If it keeps happening with the offset
 * learned, it is noise, and the window widens.
 */
static void learn_false_wake(card_reader_t *cr)
{
    cr->false_wakes++;
    cr->true_streak = 0u;
    if (cr->wake_raw != 0u) {
        int32_t off = (int32_t)cr->wake_raw - (int32_t)cr->measured;

        if (off > 64) {
            off = 64;
        } else if (off < -64) {
            off = -64;
        }
        cr->offset = (int16_t)off;
    }
    if (cr->false_streak < 0xFFu) {
        cr->false_streak++;
    }
    if (cr->false_streak >= 2u && cr->delta < APP_NFC_WAKE_DELTA_MAX) {
        cr->delta++;
    }
}

/** A wake-up that found a card: the window is not too narrow to be useful. */
static void learn_true_wake(card_reader_t *cr)
{
    cr->false_streak = 0u;
    if (cr->true_streak < 0xFFu) {
        cr->true_streak++;
    }
    if (cr->true_streak >= APP_NFC_WAKE_RELAX_AFTER && cr->delta > APP_NFC_WAKE_DELTA_MIN) {
        cr->delta--;
        cr->true_streak = 0u;
    }
}

/** Wake-up mode is for an empty field; a held card is tracked by polling. */
static bool should_arm(const card_reader_t *cr)
{
    return cr->use_wakeup && !cr->present &&
           cr->misses >= APP_NFC_REMOVE_MISSES;
}

/**
 * Measure the empty field and arm against it. The reference is taken afresh
 * every time, so drift in the cell voltage or the surroundings since the last
 * arm cannot accumulate: drift past the delta wakes the reader, finds nothing,
 * and lands back here.
 */
static bool arm(card_reader_t *cr)
{
    uint8_t measured = 0u;
    int32_t ref;

    if (cr->woken) {
        learn_false_wake(cr);   /* before cr->measured is replaced */
        cr->woken = false;
    }
    if (!plat_nfc_measure_amplitude(&measured)) {
        cr->errors++;
        return false; /* poll instead; tried again after the next empty poll */
    }
    ref = (int32_t)measured + cr->offset;
    if (ref < 0) {
        ref = 0;
    } else if (ref > 255) {
        ref = 255;
    }
    if (!plat_nfc_wakeup_arm((uint8_t)ref, cr->delta, (uint16_t)APP_NFC_WAKE_PERIOD_MS)) {
        cr->errors++;
        return false;
    }
    cr->measured = measured;
    cr->reference = (uint8_t)ref;
    cr->armed = true;
    return true;
}

void cr_enable(card_reader_t *cr, bool enable, uint32_t now_ms)
{
    if (enable == cr->enabled) {
        return;
    }
    cr->enabled = enable;

    if (enable) {
        cr->next_poll_ms = now_ms;
    } else {
        if (cr->field_on) {
            plat_nfc_field(false);
            cr->field_on = false;
        }
        if (cr->armed) {
            disarm(cr);
        }
    }
}

bool cr_busy(const card_reader_t *cr)
{
    return cr->field_on;
}

bool cr_armed(const card_reader_t *cr)
{
    return cr->armed;
}

bool cr_next_ms(const card_reader_t *cr, uint32_t *at_ms)
{
    if (!cr->enabled || cr->armed) {
        return false;
    }
    *at_ms = cr->field_on ? (cr->field_on_ms + APP_NFC_FIELD_GUARD_MS + 1u) : cr->next_poll_ms;
    return true;
}

void cr_wake(card_reader_t *cr, uint32_t now_ms)
{
    if (!cr->armed) {
        return;
    }
    disarm(cr);
    cr->wakeups++;
    cr->woken = true;
    cr->misses = 0u;
    cr->next_poll_ms = now_ms;
}

static bool same_as_present(const card_reader_t *cr, const iso14443a_card_t *card)
{
    uint8_t i;

    if (!cr->present || card->uid_len != cr->present_len) {
        return false;
    }
    for (i = 0u; i < card->uid_len; i++) {
        if (card->uid[i] != cr->present_uid[i]) {
            return false;
        }
    }
    return true;
}

bool cr_task(card_reader_t *cr, uint32_t now_ms, iso14443a_card_t *card)
{
    uint8_t i;

    if (!cr->enabled || cr->armed) {
        return false;
    }

    if (!cr->field_on) {
        if ((int32_t)(now_ms - cr->next_poll_ms) < 0) {
            return false;
        }
        plat_nfc_field(true);
        cr->field_on = true;
        cr->field_on_ms = now_ms;
        return false;
    }

    /* The millisecond tick can advance just after the field went on, so one
     * extra tick guarantees the full guard time. */
    if ((uint32_t)(now_ms - cr->field_on_ms) < (APP_NFC_FIELD_GUARD_MS + 1u)) {
        return false;
    }

    iso14443a_status_t st = iso14443a_select(card);

    plat_nfc_field(false);
    cr->field_on = false;
    cr->next_poll_ms = now_ms + APP_NFC_POLL_MS;
    cr->polls++;
    cr->last_status = st;

    switch (st) {
    case ISO14443A_OK:
        cr->misses = 0u;
        if (cr->woken) {
            learn_true_wake(cr);
            cr->woken = false;
        }
        if (same_as_present(cr, card)) {
            return false;   /* still being held there */
        }
        cr->present = true;
        cr->present_len = card->uid_len;
        for (i = 0u; i < card->uid_len; i++) {
            cr->present_uid[i] = card->uid[i];
        }
        return true;

    case ISO14443A_NO_CARD:
        if (cr->misses < MISSES_MAX) {
            cr->misses++;
        }
        if (cr->present && cr->misses >= APP_NFC_REMOVE_MISSES) {
            cr->present = false;
        }
        /* Only straight after a poll that found nothing: arming with a card
         * already on the antenna would make it part of the reference, and
         * that card would never wake the reader. */
        if (should_arm(cr)) {
            (void)arm(cr);
        }
        return false;

    case ISO14443A_COLLISION:
        /* Says nothing about whether the held card left. */
        cr->collisions++;
        return false;

    case ISO14443A_PROTOCOL:
    case ISO14443A_IO:
    default:
        cr->errors++;
        return false;
    }
}

uint32_t card_id_from_uid(const uint8_t *uid, uint8_t len)
{
    uint32_t id = 0u;
    uint8_t i = (len > 4u) ? (uint8_t)(len - 4u) : 0u;

    for (; i < len; i++) {
        id = (id << 8) | uid[i];
    }
    return id;
}
