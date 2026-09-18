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
 *   page  0        config + student-list descriptor
 *   pages 1 .. 8   student list, 4 kB entries each, sorted ascending
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

typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t student_count;   /**< Number of 32-bit IDs in the student list. */
    uint32_t student_crc32;   /**< CRC-32 over those IDs. */
    uint32_t device_id;       /**< Printed on the enclosure; appears in the CSV. */
    uint32_t reserved[3];
} nv_config_t;               /* 32 bytes = 4 double-words */

/* ---- Student list ------------------------------------------------------- */

#define NV_STUDENTS_OFFSET  (NV_PAGE_SIZE * 1u)
#define NV_STUDENTS_PAGES   8u
#define NV_STUDENTS_BYTES   (NV_PAGE_SIZE * NV_STUDENTS_PAGES)
#define NV_STUDENTS_MAX     (NV_STUDENTS_BYTES / 4u)   /* 4096 IDs */

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

/** Erased flash pattern, used to spot never-written double-words. */
#define NV_ERASED_DW        0xFFFFFFFFFFFFFFFFull

#endif /* NV_LAYOUT_H */
