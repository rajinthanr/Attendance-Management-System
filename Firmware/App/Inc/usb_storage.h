/**
 * @file    usb_storage.h
 * @brief   Level 2 (logic) — the mass-storage block device the host sees.
 *
 * Level 1's USB MSC glue calls straight into this; it holds no policy of its
 * own. The volume is synthesised sector by sector from the flash log, so the
 * only RAM cost is the one sector the USB stack is already using.
 */
#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include "app_types.h"
#include "log_store.h"
#include "fat12.h"

/**
 * Prepare a volume snapshot for one enumeration.
 *
 * The record count is latched here rather than read per sector: a host that
 * saw a file size change underneath it mid-copy would produce a truncated or
 * corrupt CSV. New scans that arrive while USB is attached stay in the RAM
 * buffer and appear on the next attach.
 *
 * @param ls        Log to export. Must outlive the USB session.
 * @param device_id Becomes the FAT volume serial.
 * @param now       Timestamp stamped on the exported file.
 */
void usbs_begin(const log_store_t *ls, uint32_t device_id, const app_datetime_t *now);

/** Sectors and sector size the MSC layer should report in READ CAPACITY. */
uint32_t usbs_sector_count(void);
uint16_t usbs_sector_size(void);

/** Bytes in the exported CSV, for progress reporting and diagnostics. */
uint32_t usbs_file_size(void);

/**
 * Read @p count consecutive sectors starting at @p lba into @p buf.
 * @return false when the range leaves the volume.
 */
bool usbs_read(uint32_t lba, uint8_t *buf, uint32_t count);

/**
 * Writes are rejected: the volume is a view of an append-only log, and there
 * is nowhere to put a modified sector. The MSC layer turns false into a
 * write-protect sense code.
 */
bool usbs_write(uint32_t lba, const uint8_t *buf, uint32_t count);

/** True once the host has actually read a data sector, i.e. copied the file. */
bool usbs_file_was_read(void);

#endif /* USB_STORAGE_H */
