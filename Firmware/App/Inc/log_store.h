/**
 * @file    log_store.h
 * @brief   Level 2 (logic) — append-only attendance log on flash.
 *
 * Owns page selection, wear ordering, CRC sealing and recovery after an
 * unexpected power loss. Reaches the flash only through plat_flash_*, so the
 * whole format is exercisable on the host.
 */
#ifndef LOG_STORE_H
#define LOG_STORE_H

#include "app_types.h"
#include "nv_layout.h"
#include "record_buffer.h"

typedef struct {
    uint8_t  order[NV_LOG_PAGES];   /**< Physical page index, oldest first. */
    uint16_t counts[NV_LOG_PAGES];  /**< Records in each ordered page. */
    uint32_t seq[NV_LOG_PAGES];     /**< Sequence number of each ordered page. */
    uint8_t  n_used;                /**< Pages holding data. */
    uint32_t total;                 /**< Records across all pages. */

    uint8_t  open_page;             /**< Physical page accepting writes, or 0xFF. */
    uint16_t open_slot;             /**< Next record slot, 1..NV_LOG_RECS_PER_PAGE. */
    uint32_t next_seq;              /**< Sequence number for the next page opened. */
    bool     full;                  /**< No erased page left to open. */
} log_store_t;

/**
 * Scan the log area and rebuild the in-RAM index.
 *
 * Recovers from a power loss mid-page: an opened-but-unsealed page is
 * adopted and appending continues after its last written record.
 */
void log_init(log_store_t *ls);

/** Records currently stored in flash. */
uint32_t log_total(const log_store_t *ls);

/** True when the log area cannot accept another page. */
bool log_is_full(const log_store_t *ls);

/** Free record slots remaining. */
uint32_t log_remaining(const log_store_t *ls);

/**
 * Move records out of the RAM buffer into flash.
 *
 * Consumes what it commits, so a partial write (log filling up mid-flush)
 * leaves the uncommitted tail in RAM instead of losing it.
 *
 * @return number of records committed.
 */
uint16_t log_flush(log_store_t *ls, record_buffer_t *rb);

/**
 * Random access by logical index, 0 = oldest. Backs the CSV generator, which
 * the USB host reads in whatever sector order it likes.
 */
bool log_read(const log_store_t *ls, uint32_t index, app_record_t *out);

/** Seal the open page. Called before Standby so no page is left unsealed. */
void log_seal(log_store_t *ls);

/** Erase every log page and reset the index. */
bool log_erase_all(log_store_t *ls);

#endif /* LOG_STORE_H */
