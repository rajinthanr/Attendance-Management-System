/**
 * @file    iso14443a.c
 * @brief   Level 2 (logic) — REQA, anticollision and SELECT.
 *
 * Only the single-card path is implemented: a collision is reported rather
 * than resolved bit by bit. An attendance reader is presented one card at a
 * time, and the next poll, 100 ms later, simply tries again.
 */
#include "iso14443a.h"
#include "platform_if.h"
#include "crc.h"

#define SEL_NVB_ANTICOLL   0x20u   /* NVB: command and NVB bytes only */
#define SEL_NVB_SELECT     0x70u   /* NVB: all 40 UID + BCC bits      */
#define CASCADE_TAG        0x88u
#define SAK_UID_INCOMPLETE 0x04u

/** A timeout after REQA means "no card"; later it means the card left. */
static iso14443a_status_t map_status(plat_nfc_status_t st)
{
    switch (st) {
    case PLAT_NFC_COLLISION:
        return ISO14443A_COLLISION;
    case PLAT_NFC_IO_ERROR:
        return ISO14443A_IO;
    case PLAT_NFC_OK:
        return ISO14443A_OK;
    case PLAT_NFC_TIMEOUT:
    case PLAT_NFC_RX_ERROR:
    default:
        return ISO14443A_PROTOCOL;
    }
}

/** SAK arrives as one byte plus CRC_A, which the reader leaves in its FIFO. */
static bool sak_valid(const uint8_t *rx, uint8_t n)
{
    if (n == 1u) {
        return true;   /* the reader already checked and stripped the CRC */
    }
    if (n != 3u) {
        return false;
    }
    uint16_t crc = crc16_iso14443a(rx, 1u);
    return rx[1] == (uint8_t)crc && rx[2] == (uint8_t)(crc >> 8);
}

iso14443a_status_t iso14443a_select(iso14443a_card_t *card)
{
    static const uint8_t sel_code[3] = { 0x93u, 0x95u, 0x97u };
    plat_nfc_status_t st;
    uint8_t level;
    uint8_t i;

    card->uid_len = 0u;
    card->sak = 0u;

    st = plat_nfc_reqa(card->atqa);
    if (st == PLAT_NFC_TIMEOUT) {
        return ISO14443A_NO_CARD;
    }
    if (st != PLAT_NFC_OK) {
        return map_status(st);
    }

    for (level = 0u; level < 3u; level++) {
        uint8_t frame[7];
        uint8_t uid_part[5];
        uint8_t sak[3];
        uint8_t n = 0u;

        /* Anticollision: ask for the whole UID part of this level. */
        frame[0] = sel_code[level];
        frame[1] = SEL_NVB_ANTICOLL;
        st = plat_nfc_transceive(frame, 2u, PLAT_NFC_ANTICOLLISION,
                                 uid_part, sizeof(uid_part), &n);
        if (st != PLAT_NFC_OK) {
            return map_status(st);
        }
        if (n != sizeof(uid_part) ||
            (uint8_t)(uid_part[0] ^ uid_part[1] ^ uid_part[2] ^ uid_part[3]) !=
                uid_part[4]) {
            return ISO14443A_PROTOCOL;
        }

        /* SELECT echoes the UID part and BCC back with a CRC. */
        frame[1] = SEL_NVB_SELECT;
        for (i = 0u; i < sizeof(uid_part); i++) {
            frame[2u + i] = uid_part[i];
        }
        st = plat_nfc_transceive(frame, sizeof(frame), PLAT_NFC_TX_CRC,
                                 sak, sizeof(sak), &n);
        if (st != PLAT_NFC_OK) {
            return map_status(st);
        }
        if (!sak_valid(sak, n)) {
            return ISO14443A_PROTOCOL;
        }

        bool cascade = (uid_part[0] == CASCADE_TAG);
        bool more = ((sak[0] & SAK_UID_INCOMPLETE) != 0u);
        if (cascade != more) {
            return ISO14443A_PROTOCOL;
        }

        uint8_t first = cascade ? 1u : 0u;
        for (i = first; i < 4u; i++) {
            card->uid[card->uid_len++] = uid_part[i];
        }

        if (!more) {
            card->sak = sak[0];
            return ISO14443A_OK;
        }
    }

    /* A fourth cascade level does not exist. */
    return ISO14443A_PROTOCOL;
}
