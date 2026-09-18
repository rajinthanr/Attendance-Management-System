/**
 * @file    usb_storage.c
 * @brief   Level 2 (logic) — synthesise the exported volume.
 *
 * LBA routing:
 *   0                      boot sector
 *   1 .. 12                FAT 1 then FAT 2 (identical)
 *   13                     root directory
 *   14 .. 2047             file data, contiguous from the first cluster
 */
#include "usb_storage.h"
#include "csv.h"

static const log_store_t *s_log;
static fat12_vol_t s_vol;
static uint32_t s_records;      /**< Latched at usbs_begin(). */
static bool s_data_read;

static void fill_zero(uint8_t *p, uint32_t n)
{
    while (n-- > 0u) {
        *p++ = 0u;
    }
}

void usbs_begin(const log_store_t *ls, uint32_t device_id, const app_datetime_t *now)
{
    s_log = ls;
    s_data_read = false;
    s_records = (ls != NULL) ? log_total(ls) : 0u;

    uint32_t size = csv_size(s_records);
    if (size > FAT12_MAX_FILE_BYTES) {
        /* Clamp rather than expose a file the FAT chain cannot describe.
         * The log area is smaller than the volume, so this cannot happen with
         * the shipped geometry; it guards a future capacity change. */
        s_records = (FAT12_MAX_FILE_BYTES / CSV_ROW_BYTES) - 1u;
        size = csv_size(s_records);
    }

    s_vol.file_size = size;
    s_vol.volume_serial = (device_id != 0u) ? device_id : 0x43415331u;
    fat12_pack_datetime(now, &s_vol.fat_date, &s_vol.fat_time);
}

uint32_t usbs_sector_count(void)
{
    return FAT12_TOTAL_SECTORS;
}

uint16_t usbs_sector_size(void)
{
    return FAT12_SECTOR_SIZE;
}

uint32_t usbs_file_size(void)
{
    return s_vol.file_size;
}

bool usbs_file_was_read(void)
{
    return s_data_read;
}

/**
 * Render one sector of file data.
 *
 * CSV_ROW_BYTES divides FAT12_SECTOR_SIZE exactly, so a sector is always a
 * whole number of rows and the first row index is a plain multiply. Row 0 of
 * the file is the CSV header; row n+1 is record n.
 */
static void read_data_sector(uint32_t file_sector, uint8_t *buf)
{
    uint32_t first_row = file_sector * CSV_ROWS_PER_SECTOR;
    uint32_t i;

    for (i = 0u; i < CSV_ROWS_PER_SECTOR; i++) {
        char *row = (char *)&buf[i * CSV_ROW_BYTES];
        uint32_t global = first_row + i;

        if (global == 0u) {
            csv_header(row);
            continue;
        }

        uint32_t record_index = global - 1u;
        app_record_t rec;

        if (record_index < s_records && s_log != NULL &&
            log_read(s_log, record_index, &rec)) {
            csv_row(&rec, row);
        } else {
            /* Past end of file. The host should not be looking here, but a
             * read-ahead will, so return zeros rather than stale data. */
            fill_zero((uint8_t *)row, CSV_ROW_BYTES);
        }
    }
}

static bool read_one(uint32_t lba, uint8_t *buf)
{
    if (lba >= FAT12_TOTAL_SECTORS) {
        return false;
    }

    if (lba < FAT12_FAT_START_LBA) {
        fat12_boot_sector(&s_vol, buf);
        return true;
    }

    if (lba < FAT12_ROOT_START_LBA) {
        /* Both FAT copies are generated from the same description. */
        uint32_t within = (lba - FAT12_FAT_START_LBA) % FAT12_SECTORS_PER_FAT;
        fat12_fat_sector(&s_vol, within, buf);
        return true;
    }

    if (lba < FAT12_DATA_START_LBA) {
        fat12_root_sector(&s_vol, lba - FAT12_ROOT_START_LBA, buf);
        return true;
    }

    s_data_read = true;
    read_data_sector(lba - FAT12_DATA_START_LBA, buf);
    return true;
}

bool usbs_read(uint32_t lba, uint8_t *buf, uint32_t count)
{
    uint32_t i;

    if ((lba + count) > FAT12_TOTAL_SECTORS) {
        return false;
    }

    for (i = 0u; i < count; i++) {
        if (!read_one(lba + i, &buf[i * FAT12_SECTOR_SIZE])) {
            return false;
        }
    }
    return true;
}

bool usbs_write(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    (void)lba;
    (void)buf;
    (void)count;
    return false;
}
