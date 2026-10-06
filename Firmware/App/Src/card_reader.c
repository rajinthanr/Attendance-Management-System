/**
 * @file    card_reader.c
 * @brief   Level 2 (logic) — reader polling and card presence tracking.
 */
#include "card_reader.h"
#include "app_config.h"
#include "platform_if.h"

void cr_init(card_reader_t *cr)
{
    uint8_t i;

    cr->enabled = false;
    cr->field_on = false;
    cr->field_on_ms = 0u;
    cr->next_poll_ms = 0u;
    cr->present = false;
    for (i = 0u; i < ISO14443A_UID_MAX; i++) {
        cr->present_uid[i] = 0u;
    }
    cr->present_len = 0u;
    cr->misses = 0u;
    cr->last_status = ISO14443A_NO_CARD;
    cr->polls = 0u;
    cr->errors = 0u;
    cr->collisions = 0u;
}

void cr_enable(card_reader_t *cr, bool enable, uint32_t now_ms)
{
    if (enable == cr->enabled) {
        return;
    }
    cr->enabled = enable;

    if (enable) {
        cr->next_poll_ms = now_ms;
    } else if (cr->field_on) {
        plat_nfc_field(false);
        cr->field_on = false;
    }
}

bool cr_busy(const card_reader_t *cr)
{
    return cr->field_on;
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

    if (!cr->enabled) {
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
        if (cr->present) {
            cr->misses++;
            if (cr->misses >= APP_NFC_REMOVE_MISSES) {
                cr->present = false;
                cr->misses = 0u;
            }
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
