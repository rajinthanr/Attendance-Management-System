/**
 * @file    fat12.h
 * @brief   Level 2 (logic) — FAT12 geometry and on-disk structure helpers.
 *
 * The volume is small and its size is chosen afresh for every USB session:
 * just big enough for the files it holds (see usb_storage.c). Only the
 * metadata lives in RAM (boot sector parameters, FAT, root directory); the
 * big file, ATTEND.CSV, is generated sector by sector and never stored.
 *
 * Geometry is fixed except for the sector count:
 *
 *   LBA 0          boot sector
 *   LBA 1..6       FAT 1   (6 sectors = 2048 twelve-bit entries)
 *   LBA 7..12      FAT 2   (identical)
 *   LBA 13         root directory (16 entries)
 *   LBA 14..       data, one sector per cluster, cluster 2 = LBA 14
 *
 * One sector per cluster keeps "which file sector is this?" a subtraction, and
 * at most 2046 data clusters (about 1 MB, and a full 32-byte-row log needs
 * under half of that) keeps the volume well under the 4085 above which a host
 * would read it as FAT16.
 */
#ifndef FAT12_H
#define FAT12_H

#include "app_types.h"

#define FAT12_SECTOR_SIZE        512u
#define FAT12_RESERVED_SECTORS   1u             /* the boot sector */
#define FAT12_NUM_FATS           2u
#define FAT12_SECTORS_PER_FAT    6u             /* 2048 entries * 1.5 B */
#define FAT12_SECTORS_PER_CLUSTER 1u
#define FAT12_ROOT_ENTRIES       16u
#define FAT12_ROOT_SECTORS       ((FAT12_ROOT_ENTRIES * 32u) / FAT12_SECTOR_SIZE)

#define FAT12_FAT_START_LBA      FAT12_RESERVED_SECTORS
#define FAT12_ROOT_START_LBA     (FAT12_FAT_START_LBA + (FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT))
#define FAT12_DATA_START_LBA     (FAT12_ROOT_START_LBA + FAT12_ROOT_SECTORS)

/** Bytes in one copy of the FAT. */
#define FAT12_FAT_BYTES          (FAT12_SECTORS_PER_FAT * FAT12_SECTOR_SIZE)

/** Entries one FAT holds, and so the highest usable cluster number + 1. */
#define FAT12_FAT_ENTRIES        ((FAT12_FAT_BYTES * 2u) / 3u)

/** Most data clusters a volume with this FAT can have (clusters 2..2047). */
#define FAT12_MAX_CLUSTERS       (FAT12_FAT_ENTRIES - 2u)

#define FAT12_EOC                0x0FFFu
#define FAT12_FIRST_CLUSTER      2u

/** Directory entry attribute bits. */
#define FAT12_ATTR_READ_ONLY     0x01u
#define FAT12_ATTR_HIDDEN        0x02u
#define FAT12_ATTR_SYSTEM        0x04u
#define FAT12_ATTR_VOLUME_ID     0x08u
#define FAT12_ATTR_DIRECTORY     0x10u
#define FAT12_ATTR_ARCHIVE       0x20u

/** Describes the volume for one enumeration. */
typedef struct {
    uint32_t total_sectors;  /**< Whole volume, boot sector included. */
    uint32_t volume_serial;  /**< Shown by the host; use the device ID. */
} fat12_vol_t;

/** Data clusters in a volume of @p total_sectors. */
uint32_t fat12_cluster_count(uint32_t total_sectors);

/** LBA of data cluster @p cluster (>= 2). */
uint32_t fat12_cluster_lba(uint32_t cluster);

/** Pack a calendar date/time into the two FAT directory-entry words. */
void fat12_pack_datetime(const app_datetime_t *dt, uint16_t *date, uint16_t *time);

/** Render sector 0. @p sector must be FAT12_SECTOR_SIZE bytes. */
void fat12_boot_sector(const fat12_vol_t *vol, uint8_t *sector);

/** Zero a FAT and set the two reserved entries (media byte, end of chain). */
void fat12_fat_init(uint8_t *fat);

/** Read / write one 12-bit entry of a FAT held in RAM. */
uint16_t fat12_get(const uint8_t *fat, uint32_t cluster);
void fat12_set(uint8_t *fat, uint32_t cluster, uint16_t value);

/** Link @p count clusters starting at @p first into one chain, ending in EOC. */
void fat12_chain(uint8_t *fat, uint32_t first, uint32_t count);

/** Fill one 32-byte directory entry. @p name is the 11-byte 8.3 form. */
void fat12_dirent(uint8_t *d, const char name[11], uint8_t attr,
                  uint16_t first_cluster, uint32_t size,
                  uint16_t date, uint16_t time);

/** Little endian accessors for directory entries read back from the host. */
uint16_t fat12_rd16(const uint8_t *p);
uint32_t fat12_rd32(const uint8_t *p);

#endif /* FAT12_H */
