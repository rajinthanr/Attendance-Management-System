/**
 * @file    iso14443a.h
 * @brief   Level 2 (logic) — ISO/IEC 14443-3 Type A card activation.
 *
 * REQA, then anticollision and SELECT for each cascade level, until the card
 * reports its UID complete. The reader only moves frames (plat_nfc_*); the
 * protocol decisions, the BCC check and the SAK's CRC_A check are made here,
 * so the whole exchange runs on the host against a simulated card.
 *
 *   REQA                      -> ATQA (2 bytes)
 *   93/95/97 20               -> UID CLn (4 bytes) + BCC
 *   93/95/97 70 UID BCC CRC   -> SAK + CRC_A
 *
 * A first UID byte of 0x88 is the cascade tag: three UID bytes follow and the
 * SAK's bit 2 says another level is coming. 4-, 7- and 10-byte UIDs result.
 */
#ifndef ISO14443A_H
#define ISO14443A_H

#include "app_types.h"

#define ISO14443A_UID_MAX   10u

typedef enum {
    ISO14443A_OK = 0,
    ISO14443A_NO_CARD,     /**< Nothing answered REQA. */
    ISO14443A_COLLISION,   /**< More than one card in the field. */
    ISO14443A_PROTOCOL,    /**< A card answered but the exchange broke down. */
    ISO14443A_IO           /**< The reader itself failed. */
} iso14443a_status_t;

typedef struct {
    uint8_t uid[ISO14443A_UID_MAX];
    uint8_t uid_len;   /**< 4, 7 or 10. */
    uint8_t atqa[2];
    uint8_t sak;       /**< SAK of the last cascade level. */
} iso14443a_card_t;

/**
 * Activate one card and read its full UID, leaving it in the ACTIVE state.
 * The field must have been on for the guard time already.
 */
iso14443a_status_t iso14443a_select(iso14443a_card_t *card);

#endif /* ISO14443A_H */
