/**
 * @file    device_cfg.c
 * @brief   Level 2 (logic) — the device ID and card list in flash.
 */
#include "device_cfg.h"
#include "platform_if.h"
#include "crc.h"

/* ------------------------------------------------------------------------ */
/* Load                                                                     */
/* ------------------------------------------------------------------------ */

void devcfg_load(device_cfg_t *c)
{
    nv_config_t raw;

    c->valid = false;
    c->device_id = 0u;
    c->card_count = 0u;
    c->card_crc = 0u;

    if (!plat_flash_read(NV_CONFIG_OFFSET, &raw, sizeof(raw))) {
        return;
    }
    if (raw.magic != NV_CONFIG_MAGIC || raw.format_version != NV_CONFIG_FORMAT) {
        return;     /* blank, or written by firmware with another layout */
    }
    c->valid = true;
    c->device_id = raw.device_id;
    if (raw.card_count <= NV_CARDS_MAX) {
        c->card_count = raw.card_count;
        c->card_crc = raw.card_crc32;
    }
}

/* ------------------------------------------------------------------------ */
/* Store                                                                    */
/* ------------------------------------------------------------------------ */

/** Program the config page: last double-word first, so the magic goes in last. */
static bool write_config(uint32_t device_id, uint32_t count, uint32_t crc)
{
    const uint32_t words[8] = { NV_CONFIG_MAGIC, NV_CONFIG_FORMAT, device_id, count, crc, 0u, 0u, 0u };
    uint8_t raw[sizeof(nv_config_t)];
    uint32_t i, b;

    /* Serialised by hand, little endian, so no struct padding is involved. */
    for (i = 0u; i < sizeof(raw); i++) {
        raw[i] = (uint8_t)((words[i / 4u] >> (8u * (i % 4u))) & 0xFFu);
    }
    for (i = sizeof(raw); i > 0u; i -= 8u) {
        uint64_t dw = 0u;

        for (b = 0u; b < 8u; b++) {
            dw |= (uint64_t)raw[(i - 8u) + b] << (8u * b);
        }
        if (!plat_flash_write_dw(NV_CONFIG_OFFSET + (i - 8u), dw)) {
            return false;
        }
    }
    return true;
}

/** True when the config page holds exactly these words. */
static bool config_reads_back(uint32_t device_id, uint32_t count, uint32_t crc)
{
    const uint32_t words[8] = { NV_CONFIG_MAGIC, NV_CONFIG_FORMAT, device_id, count, crc, 0u, 0u, 0u };
    uint8_t back[sizeof(nv_config_t)];
    uint32_t i;

    /* Compare the whole page, not just the fields in use: a bit stored wrong
     * anywhere is a failure. */
    if (!plat_flash_read(NV_CONFIG_OFFSET, back, sizeof(back))) {
        return false;
    }
    for (i = 0u; i < sizeof(back); i++) {
        if (back[i] != (uint8_t)((words[i / 4u] >> (8u * (i % 4u))) & 0xFFu)) {
            return false;
        }
    }
    return true;
}

bool devcfg_set_device_id(device_cfg_t *c, uint32_t id)
{
    const uint32_t count = c->valid ? c->card_count : 0u;
    const uint32_t crc = c->valid ? c->card_crc : 0u;
    bool ok;

    if (!plat_flash_erase(NV_CONFIG_OFFSET)) {
        devcfg_load(c);
        return false;
    }
    ok = write_config(id, count, crc);
    devcfg_load(c);
    return ok && config_reads_back(id, count, crc) && c->valid && c->device_id == id;
}

bool devcfg_set_cards(device_cfg_t *c, uint32_t device_id, uint32_t count,
                      uint32_t crc, devcfg_card_feed_fn feed, void *ctx)
{
    uint32_t page, i;
    uint32_t pending = 0u;
    bool have_pending = false;
    bool ok = true;

    if (count > NV_CARDS_MAX) {
        return false;
    }

    /* Invalidate first. Until the config is written again the device has no
     * list, which means no card is called unknown. */
    if (!plat_flash_erase(NV_CONFIG_OFFSET)) {
        devcfg_load(c);
        return false;
    }
    for (page = 0u; page < NV_CARDS_PAGES; page++) {
        if (!plat_flash_erase(NV_CARDS_OFFSET + (page * NV_PAGE_SIZE))) {
            devcfg_load(c);
            return false;
        }
    }

    /* Two 32-bit numbers per double-word; an odd last one is padded with the
     * erased pattern, which the count ignores. */
    for (i = 0u; i < count && ok; i++) {
        uint32_t id;

        if (!feed(ctx, &id)) {
            ok = false;
            break;
        }
        if (!have_pending) {
            pending = id;
            have_pending = true;
        } else {
            uint64_t dw = (uint64_t)pending | ((uint64_t)id << 32);

            ok = plat_flash_write_dw(NV_CARDS_OFFSET + (((i - 1u) / 2u) * 8u), dw);
            have_pending = false;
        }
    }
    if (ok && have_pending) {
        uint64_t dw = (uint64_t)pending | ((uint64_t)0xFFFFFFFFu << 32);

        ok = plat_flash_write_dw(NV_CARDS_OFFSET + ((count / 2u) * 8u), dw);
    }
    if (ok) {
        ok = write_config(device_id, count, crc);
    }
    devcfg_load(c);
    return ok && config_reads_back(device_id, count, crc) && c->valid &&
           c->card_count == count && (count == 0u || cards_verify(c));
}

/* ------------------------------------------------------------------------ */
/* The list                                                                 */
/* ------------------------------------------------------------------------ */

uint32_t cards_crc_update(uint32_t crc, uint32_t id)
{
    uint8_t b[4];

    b[0] = (uint8_t)(id & 0xFFu);
    b[1] = (uint8_t)((id >> 8) & 0xFFu);
    b[2] = (uint8_t)((id >> 16) & 0xFFu);
    b[3] = (uint8_t)((id >> 24) & 0xFFu);
    return crc32_ieee_update(crc, b, 4u);
}

uint32_t cards_crc_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

bool cards_read(const device_cfg_t *c, uint32_t index, uint32_t *id)
{
    if (!c->valid || index >= c->card_count) {
        return false;
    }
    return plat_flash_read(NV_CARDS_OFFSET + (index * 4u), id, sizeof(*id));
}

bool cards_verify(const device_cfg_t *c)
{
    uint32_t crc = CARDS_CRC_INIT;
    uint32_t i;

    if (!c->valid || c->card_count == 0u) {
        return false;
    }
    for (i = 0u; i < c->card_count; i++) {
        uint32_t id;

        if (!cards_read(c, i, &id)) {
            return false;
        }
        crc = cards_crc_update(crc, id);
    }
    return cards_crc_final(crc) == c->card_crc;
}

bool cards_is_known(const device_cfg_t *c, uint32_t id)
{
    uint32_t low = 0u;
    uint32_t high = c->card_count;     /* exclusive */

    if (!c->valid || c->card_count == 0u) {
        return true;                   /* no list: nothing to compare with */
    }
    while (low < high) {
        uint32_t mid = low + ((high - low) / 2u);
        uint32_t value;

        if (!cards_read(c, mid, &value)) {
            return true;               /* unreadable flash is not evidence of a stranger */
        }
        if (value == id) {
            return true;
        }
        if (value < id) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return false;
}
