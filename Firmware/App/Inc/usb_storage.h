/**
 * @file    usb_storage.h
 * @brief   Level 2 (logic) — the mass-storage block device the host sees.
 *
 * Level 1's USB MSC glue calls straight into this; it holds no policy of its
 * own. The volume holds:
 *
 *   ATTEND.CSV    read-only. One row per tap, DATE,TIME,CARD_ID, synthesised
 *                 sector by sector from the flash log, so it costs no RAM
 *                 however long the log is. Session markers in the log are not
 *                 rows (see session.h).
 *   LECTURES.CSV  read-only. One row per lecture marker (csv_lecture_row()).
 *   SETTINGS.CSV  read/write, a few hundred bytes: the clock, the module and
 *                 lecture being taught, the device ID. The host edits it; the
 *                 changes are checked and applied by usbs_end() when the host
 *                 goes away.
 *   STATUS.TXT    read-only. A few lines on what the device holds and whether
 *                 the host's current SETTINGS.CSV would be accepted.
 *   LASTCARD.TXT  read-only, one sector, rendered at every read: the last card
 *                 read during this USB session (usbs_set_last_card()), so the
 *                 PC app can register a card while the device is plugged in.
 *   LECTURES\     read-only folder: ATTEND.CSV split by lecture, one file per
 *                 lecture marker, "L001_2026-10-07_14-30.csv" (number, then
 *                 the start date and time; 8.3 alias L001.CSV). Lectures are
 *                 numbered from 1 in log order, so from 1 again after the log
 *                 is cleared. Taps logged before the first lecture, if any,
 *                 are L000, named after the first of them.
 *
 * Cluster map (one sector per cluster):
 *
 *   2          STATUS.TXT
 *   3 .. 30    SETTINGS.CSV window (28 clusters, the only space left free)
 *   31         LASTCARD.TXT
 *   32 ..      ATTEND.CSV, then LECTURES.CSV, then the LECTURES directory,
 *              then the lecture files
 *
 * The volume is exactly as large as those areas, so the only free space the
 * host can see is the window. Anything it writes there lands in RAM, and
 * nothing it writes to a file is accepted. Writes to the LECTURES directory
 * (a host updating an access date) are accepted and dropped.
 */
#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include "app_types.h"
#include "device_cfg.h"
#include "log_store.h"
#include "settings_file.h"
#include "fat12.h"

#define USBS_STATUS_CLUSTER     2u
#define USBS_SETTINGS_CLUSTER   3u
#define USBS_SETTINGS_CLUSTERS  (SETF_MAX_BYTES / FAT12_SECTOR_SIZE)
#define USBS_LASTCARD_CLUSTER   (USBS_SETTINGS_CLUSTER + USBS_SETTINGS_CLUSTERS)
#define USBS_ATTEND_CLUSTER     (USBS_LASTCARD_CLUSTER + 1u)

/** Longest UID LASTCARD.TXT shows (ISO14443-A triple size). */
#define USBS_UID_MAX            10u

typedef enum {
    USBS_IMPORT_NONE = 0,   /**< The host did not change SETTINGS.CSV. */
    USBS_IMPORT_OK,         /**< New settings were validated and applied. */
    USBS_IMPORT_FAILED      /**< Refused; see @c rep.status. Nothing was applied. */
} usbs_import_t;

typedef struct {
    usbs_import_t outcome;
    setf_report_t rep;        /**< Valid when outcome != NONE. */
    bool          time_set;   /**< The RTC was set from a #TIME line. */

    /** A new device ID was stored in flash; the caller should reload the config. */
    bool          device_set;
    uint32_t      device_id;

    /**
     * The host named a new lecture (or asked for one). The caller appends a
     * marker for @c module / @c lecture to the log with sess_encode(), after
     * the USB peripheral has stopped. Valid when outcome == USBS_IMPORT_OK.
     */
    bool          session_start;
    char          module[SESS_MODULE_MAX + 1u];
    char          lecture[SESS_LECTURE_MAX + 1u];

    /**
     * The host asked for the log to be erased (#CLEARLOG). The caller does it
     * after the USB peripheral has stopped, before any session marker.
     * Valid when outcome == USBS_IMPORT_OK.
     */
    bool          clear_log;
} usbs_result_t;

/**
 * Prepare a volume snapshot for one enumeration.
 *
 * The record count is latched here rather than read per sector: a host that
 * saw a file size change underneath it mid-copy would produce a truncated or
 * corrupt CSV. Scans taken while USB is attached stay in the RAM buffer and
 * appear on the next attach.
 *
 * @param ls         Log to export. Must outlive the USB session.
 * @param cfg        Device ID: the FAT volume serial and the #DEVICE line. Copied.
 * @param now        Timestamp stamped on the files, and shown on the #TIME line.
 */
void usbs_begin(const log_store_t *ls, const device_cfg_t *cfg, const app_datetime_t *now);

/**
 * End the session: if the host changed SETTINGS.CSV, validate it and apply it
 * (set the clock, store a new device ID, report a new lecture). Call it after
 * the USB peripheral is stopped, so a flash erase cannot stall USB.
 */
void usbs_end(usbs_result_t *result);

/** Sectors and sector size the MSC layer should report in READ CAPACITY. */
uint32_t usbs_sector_count(void);
uint16_t usbs_sector_size(void);

/** Bytes in the exported CSV, for progress reporting and diagnostics. */
uint32_t usbs_file_size(void);

/** Bytes in SETTINGS.CSV as first shown to the host. */
uint32_t usbs_settings_size(void);

/**
 * Read @p count consecutive sectors starting at @p lba into @p buf.
 * @return false when the range leaves the volume.
 */
bool usbs_read(uint32_t lba, uint8_t *buf, uint32_t count);

/**
 * Accept @p count sectors from the host.
 *
 * FAT, root directory and the SETTINGS.CSV window are held in RAM; a write to
 * the boot sector is accepted and ignored. A write to ATTEND.CSV or
 * STATUS.TXT is refused, and the MSC layer reports that as a write error.
 */
bool usbs_write(uint32_t lba, const uint8_t *buf, uint32_t count);

/** True once the host has actually read a data sector, i.e. copied the file. */
bool usbs_file_was_read(void);

/**
 * A card was read while the drive is up: show it in LASTCARD.TXT and count
 * it. Main loop only; the update is atomic against the USB interrupt.
 */
void usbs_set_last_card(uint32_t id, const uint8_t *uid, uint8_t uid_len);

/** Cards counted by usbs_set_last_card() since usbs_begin(). */
uint32_t usbs_last_card_taps(void);

/**
 * The cell, for STATUS.TXT's "Battery" line: millivolts and a percentage, or
 * 0 mV for "unknown". Main loop only. Kept across sessions.
 */
void usbs_set_battery(uint32_t mv, uint8_t percent);

/** Files in the LECTURES folder this session. */
uint32_t usbs_lecture_file_count(void);

#endif /* USB_STORAGE_H */
