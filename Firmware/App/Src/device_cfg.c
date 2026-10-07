/**
 * @file    device_cfg.c
 * @brief   Level 2 (logic) — the device ID in flash.
 */
#include "device_cfg.h"
#include "platform_if.h"

/* ------------------------------------------------------------------------ */
/* Load                                                                     */
/* ------------------------------------------------------------------------ */

void devcfg_load(device_cfg_t *c)
{
    nv_config_t raw;

    c->valid = false;
    c->device_id = 0u;

    if (!plat_flash_read(NV_CONFIG_OFFSET, &raw, sizeof(raw))) {
        return;
    }
    if (raw.magic != NV_CONFIG_MAGIC || raw.format_version != NV_CONFIG_FORMAT) {
        return;     /* blank, or written by firmware with another layout */
    }
    c->valid = true;
    c->device_id = raw.device_id;
}

/* ------------------------------------------------------------------------ */
/* Store                                                                    */
/* ------------------------------------------------------------------------ */

/** The page's eight words, as written. The old card-list fields stay zero. */
static void config_words(uint32_t device_id, uint32_t words[8])
{
    uint32_t i;

    for (i = 0u; i < 8u; i++) {
        words[i] = 0u;
    }
    words[0] = NV_CONFIG_MAGIC;
    words[1] = NV_CONFIG_FORMAT;
    words[2] = device_id;
}

/** Program the config page: last double-word first, so the magic goes in last. */
static bool write_config(uint32_t device_id)
{
    uint32_t words[8];
    uint8_t raw[sizeof(nv_config_t)];
    uint32_t i, b;

    config_words(device_id, words);
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
static bool config_reads_back(uint32_t device_id)
{
    uint32_t words[8];
    uint8_t back[sizeof(nv_config_t)];
    uint32_t i;

    config_words(device_id, words);
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
    bool ok;

    if (!plat_flash_erase(NV_CONFIG_OFFSET)) {
        devcfg_load(c);
        return false;
    }
    ok = write_config(id);
    devcfg_load(c);
    return ok && config_reads_back(id) && c->valid && c->device_id == id;
}
