/**
 * @file    usb_storage.h
 * @brief   Level 2 (logic) — the mass-storage block device the host sees.
 *
 * Level 1's USB MSC glue calls straight into this; it holds no policy of its
 * own. The volume holds three files:
 *
 *   ATTEND.CSV    read-only. One row per tap, DATE,TIME,CARD_ID, synthesised
 *                 sector by sector from the flash log, so it costs no RAM
 *                 however long the log is. Session markers in the log are not
 *                 rows (see session.h).
 *   SETTINGS.CSV  read/write, a few hundred bytes: the clock, the module and
 *                 lecture being taught, the device ID. The host edits it; the
 *                 changes are checked and applied by usbs_end() when the host
 *                 goes away.
 *   STATUS.TXT    read-only. A few lines on what the device holds and whether
 *                 the host's current SETTINGS.CSV would be accepted.
 *
 * Cluster map (one sector per cluster):
 *
 *   2          STATUS.TXT
 *   3 .. 30    SETTINGS.CSV window (28 clusters, the only space left free)
 *   31 ..      ATTEND.CSV
 *
 * The volume is exactly as large as those three areas, so the only free space
 * the host can see is the window. Anything it writes there lands in RAM, and
 * nothing it writes anywhere else is accepted.
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
#define USBS_ATTEND_CLUSTER     (USBS_SETTINGS_CLUSTER + USBS_SETTINGS_CLUSTERS)

typedef enum {
    USBS_IMPORT_NONE = 0,   /**< The host did not change SETTINGS.CSV. */
    USBS_IMPORT_OK,         /**< New settings were validated and applied. */
    USBS_IMPORT_FAILED      /**< Refused; see @c rep.status. Nothing was applied. */
} usbs_import_t;

typedef struct {
    usbs_import_t outcome;
    setf_report_t rep;        /**< Valid when outcome != NONE. */
    bool          time_set;   /**< The RTC was set from a #TIME line. */

    /** A new device ID or card list was stored in flash; the caller should reload the config. */
    bool          device_set;
    uint32_t      device_id;
    bool          cards_set;    /**< A new card list was stored. */
    uint32_t      card_count;   /**< ...with this many cards. */

    /**
     * The host named a new lecture (or asked for one). The caller appends a
     * marker for @c module / @c lecture to the log with sess_encode(), after
     * the USB peripheral has stopped. Valid when outcome == USBS_IMPORT_OK.
     */
    bool          session_start;
    char          module[SESS_MODULE_MAX + 1u];
    char          lecture[SESS_LECTURE_MAX + 1u];
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
 * @param cfg        Device ID (the FAT volume serial and the #DEVICE line) and the
 *                   registered card list (the #CARDS lines). Copied.
 * @param now        Timestamp stamped on the files, and shown on the #TIME line.
 */
void usbs_begin(const log_store_t *ls, const device_cfg_t *cfg, const app_datetime_t *now);

/**
 * End the session: if the host changed SETTINGS.CSV, validate it and apply it
 * (set the clock, store a new device ID or card list, report a new lecture). Call it after
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

#endif /* USB_STORAGE_H */
