/**
 * @file    device_cfg.h
 * @brief   Level 2 (logic) — what the device keeps in flash besides the log:
 *          its ID and the list of registered card numbers.
 *
 * The list holds numbers only. Who a card belongs to, and which modules they
 * take, live on the PC. The device uses the list for one thing: telling a
 * known card (green) from an unknown one (red) at the moment of the tap. The
 * card is recorded either way.
 *
 * The ID is what the host sees as the volume serial number and what the app
 * uses to tell units apart.
 */
#ifndef DEVICE_CFG_H
#define DEVICE_CFG_H

#include "app_types.h"
#include "nv_layout.h"

typedef struct {
    bool     valid;       /**< The config page holds a readable configuration. */
    uint32_t device_id;   /**< 0 when unset. */
    uint32_t card_count;  /**< Registered cards; 0 = no list, so no card is "unknown". */
    uint32_t card_crc;    /**< CRC-32 stored for the list. */
} device_cfg_t;

/** Source of card numbers to store: the next one in ascending order, or false at the end. */
typedef bool (*devcfg_card_feed_fn)(void *ctx, uint32_t *id);

/**
 * Read the config page. A blank or old-format page gives valid == false, id 0
 * and no cards. A list whose count is out of range reads as no list.
 */
void devcfg_load(device_cfg_t *c);

/**
 * Replace the device ID, keeping the card list: erase the config page,
 * program it, read it back.
 * @return true only when the stored value reads back as @p id.
 */
bool devcfg_set_device_id(device_cfg_t *c, uint32_t id);

/**
 * Replace the card list (and set the device ID in the same pass).
 *
 * The config page is erased first and programmed last, so a power cut leaves
 * "no config" (every card counts as known), never a half-written list that
 * looks valid. @p feed is called @p count times and must give strictly
 * ascending numbers; @p crc is the CRC-32 the caller computed over the same
 * numbers as little-endian bytes (see cards_crc_update()).
 *
 * @return true only when the list reads back and verifies.
 */
bool devcfg_set_cards(device_cfg_t *c, uint32_t device_id, uint32_t count,
                      uint32_t crc, devcfg_card_feed_fn feed, void *ctx);

/* ---- The list ------------------------------------------------------------ */

/** Fold one card number into a running CRC (start from CARDS_CRC_INIT). */
#define CARDS_CRC_INIT 0xFFFFFFFFu
uint32_t cards_crc_update(uint32_t crc, uint32_t id);
uint32_t cards_crc_final(uint32_t crc);

/** The @p index-th stored number. False when out of range or unreadable. */
bool cards_read(const device_cfg_t *c, uint32_t index, uint32_t *id);

/** Re-check the stored CRC over the whole list. Boot time only. */
bool cards_verify(const device_cfg_t *c);

/**
 * Is @p id on the list? True for everything when there is no list, so a
 * device that has not been given one never calls a card unknown.
 */
bool cards_is_known(const device_cfg_t *c, uint32_t id);

#endif /* DEVICE_CFG_H */
