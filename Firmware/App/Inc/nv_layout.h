/**
 * @file    nv_layout.h
 * @brief   Level 2 (logic) — on-flash data layout.
 *
 * All offsets are relative to the start of the storage region. Level 1 adds
 * the physical base address (see bsp_flash.c), so this header stays free of
 * anything device specific and the host tests can exercise the same layout
 * against a RAM-backed flash stub.
 *
 * Region map (128 kB, 64 pages of 2 kB on STM32L432KC):
 *
 *   page  0        config: the device ID, and the size and CRC of the card list
 *   pages 1 .. 8   the registered card list: sorted 32-bit card IDs, nothing
 *                  else, searched in place. No names: who a card belongs to is
 *                  the PC's business. The device uses the list only to tell a
 *                  known card (green) from an unknown one (red) when it is
 *                  tapped; every tap is recorded either way.
 *   pages 9 .. 63  attendance log, 254 records + header + footer per page
 */
#ifndef NV_LAYOUT_H
#define NV_LAYOUT_H

#include "app_types.h"

#define NV_PAGE_SIZE        2048u
#define NV_DW_PER_PAGE      (NV_PAGE_SIZE / 8u)   /* 256 double-words */

/* ---- Config page -------------------------------------------------------- */

#define NV_CONFIG_OFFSET    0u
#define NV_CONFIG_MAGIC     0x43415331u           /* "CAS1" */

/** 1..3 = earlier layouts (a name list, then none). Only 4 is read. */
#define NV_CONFIG_FORMAT    4u

typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t device_id;       /**< Printed on the enclosure; the volume serial. */
    uint32_t card_count;      /**< Cards in the list; 0 = no list, every card counts as known. */
    uint32_t card_crc32;      /**< CRC-32/IEEE over the list's bytes (little endian). */
    uint32_t reserved[3];
} nv_config_t;               /* 32 bytes = 4 double-words */

/* ---- Registered card list ----------------------------------------------- */

#define NV_CARDS_FIRST_PAGE 1u
#define NV_CARDS_PAGES      8u
#define NV_CARDS_OFFSET     (NV_PAGE_SIZE * NV_CARDS_FIRST_PAGE)
/** Flash would hold 4096; the USB text window limits what the host can send. */
#define NV_CARDS_MAX        1000u

/* ---- Attendance log ----------------------------------------------------- */

#define NV_LOG_FIRST_PAGE   9u
#define NV_LOG_PAGES        55u
#define NV_LOG_OFFSET       (NV_PAGE_SIZE * NV_LOG_FIRST_PAGE)

/** Records per page: 256 double-words minus one header and one footer. */
#define NV_LOG_RECS_PER_PAGE  (NV_DW_PER_PAGE - 2u)    /* 254 */
#define NV_LOG_CAPACITY       (NV_LOG_RECS_PER_PAGE * NV_LOG_PAGES)  /* 13970 */

#define NV_LOG_HDR_MAGIC    0x4C475041u   /* "LGPA" */
#define NV_LOG_FTR_MAGIC    0x4C474645u   /* "LGFE" */

/** First double-word of a log page, written when the page is opened. */
typedef struct {
    uint32_t magic;
    uint32_t sequence;   /**< Monotonic; orders pages independently of position. */
} nv_log_header_t;

/** Last double-word of a log page, written when the page is sealed. */
typedef struct {
    uint16_t count;      /**< Records actually stored in this page. */
    uint16_t crc16;      /**< CRC-16/CCITT over those records. */
    uint32_t magic;
} nv_log_footer_t;

/* ---- Session markers ---------------------------------------------------- */

/**
 * A lecture session is announced inside the log itself, by a header record
 * followed by up to 15 text records that carry the module and lecture names.
 * They use the 8-byte record format, so they cost no extra flash area and
 * survive exactly as long as the attendance they describe.
 *
 *   header  {0xFFFFFFF0 | n, start stamp}    n = text records that follow (0..15)
 *   text    {0xFFFFFFD0,     4 name bytes}   "module NUL lecture NUL", NUL padded
 *
 * Card IDs from NV_ID_RESERVED_MIN up can never be enrolled, so a marker is
 * recognisable from its first word alone, scanning in either direction.
 */
#define NV_ID_RESERVED_MIN  0xFFFFFF00u
#define NV_MARK_HEADER      0xFFFFFFF0u
#define NV_MARK_TEXT        0xFFFFFFD0u

/** Erased flash pattern, used to spot never-written double-words. */
#define NV_ERASED_DW        0xFFFFFFFFFFFFFFFFull

#endif /* NV_LAYOUT_H */
