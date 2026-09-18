/**
 * @file    fat12.h
 * @brief   Level 2 (logic) — read-only FAT12 volume synthesis.
 *
 * Nothing is stored: boot sector, FATs and root directory are computed each
 * time the host asks for them. A 1 MB volume would need 1 MB of somewhere to
 * live otherwise, and the part has 64 kB of RAM and no room in flash.
 *
 * The volume holds exactly one file, laid out contiguously from the first
 * data cluster, which is what lets usb_storage turn an LBA straight into a
 * byte offset into the CSV.
 *
 * Geometry is chosen so the cluster count lands inside FAT12's range:
 *   2048 sectors - 14 of metadata = 2034 clusters, well under the 4085 limit
 *   above which a host would read the volume as FAT16.
 */
#ifndef FAT12_H
#define FAT12_H

#include "app_types.h"

#define FAT12_SECTOR_SIZE        512u
#define FAT12_TOTAL_SECTORS      2048u          /* 1 MB volume */
#define FAT12_RESERVED_SECTORS   1u             /* the boot sector */
#define FAT12_NUM_FATS           2u
#define FAT12_SECTORS_PER_FAT    6u             /* 2048 entries * 1.5 B */
#define FAT12_SECTORS_PER_CLUSTER 1u
#define FAT12_ROOT_ENTRIES       16u
#define FAT12_ROOT_SECTORS       ((FAT12_ROOT_ENTRIES * 32u) / FAT12_SECTOR_SIZE)

#define FAT12_FAT_START_LBA      FAT12_RESERVED_SECTORS
#define FAT12_ROOT_START_LBA     (FAT12_FAT_START_LBA + (FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT))
#define FAT12_DATA_START_LBA     (FAT12_ROOT_START_LBA + FAT12_ROOT_SECTORS)
#define FAT12_DATA_SECTORS       (FAT12_TOTAL_SECTORS - FAT12_DATA_START_LBA)

/** Largest file the data region can hold. */
#define FAT12_MAX_FILE_BYTES     ((uint32_t)FAT12_DATA_SECTORS * FAT12_SECTOR_SIZE)

/** Describes the single-file volume for one enumeration. */
typedef struct {
    uint32_t file_size;      /**< Bytes in the file. */
    uint16_t fat_date;       /**< Packed FAT date for the file's timestamp. */
    uint16_t fat_time;       /**< Packed FAT time. */
    uint32_t volume_serial;  /**< Shown by the host; use the device ID. */
} fat12_vol_t;

/** Pack a calendar date/time into the two FAT directory-entry words. */
void fat12_pack_datetime(const app_datetime_t *dt, uint16_t *date, uint16_t *time);

/** Render sector 0. @p sector must be FAT12_SECTOR_SIZE bytes. */
void fat12_boot_sector(const fat12_vol_t *vol, uint8_t *sector);

/** Render one sector of a FAT. @p index is 0..FAT12_SECTORS_PER_FAT-1. */
void fat12_fat_sector(const fat12_vol_t *vol, uint32_t index, uint8_t *sector);

/** Render one sector of the root directory. */
void fat12_root_sector(const fat12_vol_t *vol, uint32_t index, uint8_t *sector);

#endif /* FAT12_H */
