/**
 * @file    log_store.c
 * @brief   Level 2 (logic) — append-only log implementation.
 *
 * Design notes
 * ------------
 * A record is 8 bytes and the L4 programs 8 bytes at a time, so a record is
 * never split across a program operation. That removes the usual read-modify-
 * write hazard entirely: a power loss can only ever lose the record being
 * written, never corrupt one already stored.
 *
 * Pages are self describing. The header carries a monotonic sequence number,
 * so logical order does not depend on physical position and a page can be
 * reused anywhere in the area. The footer carries the record count and a
 * CRC-16, written only once the page is closed; an unsealed page is therefore
 * unambiguously "was being written when power went away".
 */
#include "log_store.h"
#include "platform_if.h"
#include "crc.h"
#include "app_config.h"

/** Overwrite the oldest data when the log area fills, instead of refusing.
 *  Default off: silently discarding attendance history is worse than a device
 *  that visibly complains until it is emptied over USB. */
#ifndef APP_LOG_WRAP_WHEN_FULL
#define APP_LOG_WRAP_WHEN_FULL  0
#endif

#define PAGE_NONE  0xFFu

/* ------------------------------------------------------------------------ */
/* Address helpers                                                          */
/* ------------------------------------------------------------------------ */

static uint32_t page_offset(uint8_t page)
{
    return NV_LOG_OFFSET + ((uint32_t)page * NV_PAGE_SIZE);
}

static uint32_t slot_offset(uint8_t page, uint16_t slot)
{
    return page_offset(page) + ((uint32_t)slot * 8u);
}

static uint32_t footer_offset(uint8_t page)
{
    return page_offset(page) + ((NV_DW_PER_PAGE - 1u) * 8u);
}

/* ------------------------------------------------------------------------ */
/* Page state                                                               */
/* ------------------------------------------------------------------------ */

typedef enum {
    PS_ERASED = 0,
    PS_OPEN,      /**< Header valid, footer still erased. */
    PS_SEALED,    /**< Header and footer valid. */
    PS_BAD        /**< Anything else; treated as erasable. */
} page_state_t;

static page_state_t page_probe(uint8_t page, nv_log_header_t *hdr, nv_log_footer_t *ftr)
{
    uint64_t raw;

    if (!plat_flash_read(page_offset(page), &raw, sizeof(raw))) {
        return PS_BAD;
    }
    if (raw == NV_ERASED_DW) {
        return PS_ERASED;
    }

    if (!plat_flash_read(page_offset(page), hdr, sizeof(*hdr))) {
        return PS_BAD;
    }
    if (hdr->magic != NV_LOG_HDR_MAGIC) {
        return PS_BAD;
    }

    if (!plat_flash_read(footer_offset(page), &raw, sizeof(raw))) {
        return PS_BAD;
    }
    if (raw == NV_ERASED_DW) {
        return PS_OPEN;
    }

    if (!plat_flash_read(footer_offset(page), ftr, sizeof(*ftr))) {
        return PS_BAD;
    }
    if (ftr->magic != NV_LOG_FTR_MAGIC || ftr->count > NV_LOG_RECS_PER_PAGE) {
        return PS_BAD;
    }

    return PS_SEALED;
}

/** Count records in an unsealed page by finding the first erased slot. */
static uint16_t page_scan_open(uint8_t page)
{
    uint16_t slot;

    for (slot = 1u; slot <= NV_LOG_RECS_PER_PAGE; slot++) {
        uint64_t dw;

        if (!plat_flash_read(slot_offset(page, slot), &dw, sizeof(dw))) {
            break;
        }
        if (dw == NV_ERASED_DW) {
            break;
        }
    }
    return (uint16_t)(slot - 1u);   /* records present */
}

/** Insert a page into the sequence-ordered index (insertion sort; <=55 items). */
static void index_insert(log_store_t *ls, uint8_t page, uint32_t seq, uint16_t count)
{
    uint8_t i = ls->n_used;

    if (i >= NV_LOG_PAGES) {
        return;
    }

    while (i > 0u && ls->seq[i - 1u] > seq) {
        ls->seq[i]    = ls->seq[i - 1u];
        ls->order[i]  = ls->order[i - 1u];
        ls->counts[i] = ls->counts[i - 1u];
        i--;
    }
    ls->seq[i]    = seq;
    ls->order[i]  = page;
    ls->counts[i] = count;
    ls->n_used++;
    ls->total += count;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void log_init(log_store_t *ls)
{
    uint8_t page;

    ls->n_used = 0u;
    ls->total = 0u;
    ls->open_page = PAGE_NONE;
    ls->open_slot = 1u;
    ls->next_seq = 1u;
    ls->full = false;

    for (page = 0u; page < NV_LOG_PAGES; page++) {
        nv_log_header_t hdr = { 0u, 0u };
        nv_log_footer_t ftr = { 0u, 0u, 0u };

        switch (page_probe(page, &hdr, &ftr)) {
        case PS_SEALED:
            index_insert(ls, page, hdr.sequence, ftr.count);
            if (hdr.sequence >= ls->next_seq) {
                ls->next_seq = hdr.sequence + 1u;
            }
            break;

        case PS_OPEN: {
            /* Interrupted write. Adopt it and continue after the last record;
             * at most one such page can exist because only one is ever open. */
            uint16_t n = page_scan_open(page);
            index_insert(ls, page, hdr.sequence, n);
            if (hdr.sequence >= ls->next_seq) {
                ls->next_seq = hdr.sequence + 1u;
            }
            if (ls->open_page == PAGE_NONE) {
                ls->open_page = page;
                ls->open_slot = (uint16_t)(n + 1u);
            }
            break;
        }

        case PS_BAD:
            /* Garbage from a botched erase; reclaim it. */
            (void)plat_flash_erase(page_offset(page));
            break;

        case PS_ERASED:
        default:
            break;
        }
    }
}

uint32_t log_total(const log_store_t *ls)
{
    return ls->total;
}

bool log_is_full(const log_store_t *ls)
{
    return ls->full;
}

uint32_t log_remaining(const log_store_t *ls)
{
    if (ls->total >= NV_LOG_CAPACITY) {
        return 0u;
    }
    return NV_LOG_CAPACITY - ls->total;
}

/** Compute and program the footer of the currently open page. */
void log_seal(log_store_t *ls)
{
    if (ls->open_page == PAGE_NONE) {
        return;
    }

    uint16_t count = (uint16_t)(ls->open_slot - 1u);
    uint16_t crc = 0xFFFFu;
    uint16_t slot;

    for (slot = 1u; slot <= count; slot++) {
        app_record_t rec;
        if (plat_flash_read(slot_offset(ls->open_page, slot), &rec, sizeof(rec))) {
            crc = crc16_ccitt_update(crc, &rec, sizeof(rec));
        }
    }

    nv_log_footer_t ftr;
    ftr.count = count;
    ftr.crc16 = crc;
    ftr.magic = NV_LOG_FTR_MAGIC;

    uint64_t dw;
    /* Copy through a byte-wise move rather than a cast, so this stays free of
     * alignment and strict-aliasing assumptions on the host build too. */
    const uint8_t *src = (const uint8_t *)&ftr;
    uint8_t *dst = (uint8_t *)&dw;
    uint8_t i;
    for (i = 0u; i < 8u; i++) {
        dst[i] = src[i];
    }

    (void)plat_flash_write_dw(footer_offset(ls->open_page), dw);

    ls->open_page = PAGE_NONE;
    ls->open_slot = 1u;
}

/** Find an erased page and open it. Returns false when the area is full. */
static bool open_new_page(log_store_t *ls)
{
    uint8_t page;

    log_seal(ls);

    for (page = 0u; page < NV_LOG_PAGES; page++) {
        nv_log_header_t hdr = { 0u, 0u };
        nv_log_footer_t ftr = { 0u, 0u, 0u };

        if (page_probe(page, &hdr, &ftr) != PS_ERASED) {
            continue;
        }
        goto found;
    }

#if APP_LOG_WRAP_WHEN_FULL
    /* Recycle the lowest-sequence page. */
    if (ls->n_used > 0u) {
        page = ls->order[0];
        if (!plat_flash_erase(page_offset(page))) {
            ls->full = true;
            return false;
        }
        ls->total -= ls->counts[0];
        {
            uint8_t i;
            for (i = 1u; i < ls->n_used; i++) {
                ls->order[i - 1u]  = ls->order[i];
                ls->counts[i - 1u] = ls->counts[i];
                ls->seq[i - 1u]    = ls->seq[i];
            }
            ls->n_used--;
        }
        goto found;
    }
#endif

    ls->full = true;
    return false;

found:
    {
        nv_log_header_t nh;
        nh.magic = NV_LOG_HDR_MAGIC;
        nh.sequence = ls->next_seq;

        uint64_t dw;
        const uint8_t *src = (const uint8_t *)&nh;
        uint8_t *dst = (uint8_t *)&dw;
        uint8_t i;
        for (i = 0u; i < 8u; i++) {
            dst[i] = src[i];
        }

        if (!plat_flash_write_dw(page_offset(page), dw)) {
            ls->full = true;
            return false;
        }

        index_insert(ls, page, ls->next_seq, 0u);
        ls->next_seq++;
        ls->open_page = page;
        ls->open_slot = 1u;
        ls->full = false;
    }
    return true;
}

/** Bump the record count of the open page in the RAM index. */
static void index_bump_open(log_store_t *ls)
{
    uint8_t i;

    for (i = 0u; i < ls->n_used; i++) {
        if (ls->order[i] == ls->open_page) {
            ls->counts[i]++;
            ls->total++;
            return;
        }
    }
}

uint16_t log_flush(log_store_t *ls, record_buffer_t *rb)
{
    uint16_t written = 0u;
    uint16_t pending = rb_count(rb);

    while (written < pending) {
        if (ls->open_page == PAGE_NONE || ls->open_slot > NV_LOG_RECS_PER_PAGE) {
            if (!open_new_page(ls)) {
                break;   /* full: leave the rest in RAM */
            }
        }

        const app_record_t *rec = rb_peek(rb, written);
        if (rec == NULL) {
            break;
        }

        uint64_t dw;
        const uint8_t *src = (const uint8_t *)rec;
        uint8_t *dst = (uint8_t *)&dw;
        uint8_t i;
        for (i = 0u; i < 8u; i++) {
            dst[i] = src[i];
        }

        if (!plat_flash_write_dw(slot_offset(ls->open_page, ls->open_slot), dw)) {
            break;
        }

        ls->open_slot++;
        index_bump_open(ls);
        written++;
    }

    rb_consume(rb, written);
    return written;
}

bool log_read(const log_store_t *ls, uint32_t index, app_record_t *out)
{
    uint8_t i;

    if (index >= ls->total) {
        return false;
    }

    for (i = 0u; i < ls->n_used; i++) {
        if (index < ls->counts[i]) {
            /* Slot numbering starts at 1: slot 0 is the page header. */
            return plat_flash_read(slot_offset(ls->order[i], (uint16_t)(index + 1u)),
                                   out, sizeof(*out));
        }
        index -= ls->counts[i];
    }
    return false;
}

bool log_erase_all(log_store_t *ls)
{
    uint8_t page;
    bool ok = true;

    for (page = 0u; page < NV_LOG_PAGES; page++) {
        if (!plat_flash_erase(page_offset(page))) {
            ok = false;
        }
    }

    log_init(ls);
    return ok;
}
