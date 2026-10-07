/**
 * @file    fat12.h
 * @brief   Level 2 (logic) — FAT12 geometry and on-disk structure helpers.
 *
 * The volume is small and its size is chosen afresh for every USB session:
 * just big enough for the files it holds (see usb_storage.c). Only the
 * metadata lives in RAM (boot sector parameters, FAT, root directory); the
 * big files (ATTEND.CSV, the per-lecture files) are generated sector by
 * sector and never stored.
 *
 * Geometry is fixed except for the sector count:
 *
 *   LBA 0          boot sector
 *   LBA 1..12      FAT 1   (12 sectors = 4096 twelve-bit entries)
 *   LBA 13..24     FAT 2   (identical)
 *   LBA 25         root directory (16 entries)
 *   LBA 26..       data, one sector per cluster, cluster 2 = LBA 26
 *
 * One sector per cluster keeps "which file sector is this?" a subtraction.
 * A full log appears twice (ATTEND.CSV and the per-lecture files), about
 * 1750 clusters with the lecture list and the directory, so the FAT allows up
 * to 4084: the most a FAT12 volume may have before a host reads it as FAT16.
 */
#ifndef FAT12_H
#define FAT12_H

#include "app_types.h"

#define FAT12_SECTOR_SIZE        512u
#define FAT12_RESERVED_SECTORS   1u             /* the boot sector */
#define FAT12_NUM_FATS           2u
#define FAT12_SECTORS_PER_FAT    12u            /* 4096 entries * 1.5 B */
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

/** Most data clusters a volume may have: FAT12 ends at 4084, below the
 *  4096 - 2 this FAT could describe. */
#define FAT12_MAX_CLUSTERS       4084u

#define FAT12_EOC                0x0FFFu
#define FAT12_FIRST_CLUSTER      2u

/** Directory entry attribute bits. */
#define FAT12_ATTR_READ_ONLY     0x01u
#define FAT12_ATTR_HIDDEN        0x02u
#define FAT12_ATTR_SYSTEM        0x04u
#define FAT12_ATTR_VOLUME_ID     0x08u
#define FAT12_ATTR_DIRECTORY     0x10u
#define FAT12_ATTR_ARCHIVE       0x20u
#define FAT12_ATTR_LFN           0x0Fu    /**< A long-name entry. */

/** Characters one long-name entry carries (UCS-2). */
#define FAT12_LFN_CHARS          13u

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

/** The checksum a long-name entry carries of its 8.3 entry's name. */
uint8_t fat12_sfn_checksum(const char name[11]);

/**
 * Fill one long-name entry: characters (ord - 1) * 13 onwards of @p name
 * (ASCII, @p len of them). @p ord counts from 1; the entry for the end of the
 * name gets the last-entry flag. The entries go into the directory highest
 * @p ord first, followed by the 8.3 entry.
 */
void fat12_lfn_entry(uint8_t *d, uint8_t ord, uint8_t checksum,
                     const char *name, uint32_t len);

/** Long-name entries a name of @p len characters needs. */
uint32_t fat12_lfn_count(uint32_t len);

/** Little endian accessors for directory entries read back from the host. */
uint16_t fat12_rd16(const uint8_t *p);
uint32_t fat12_rd32(const uint8_t *p);

#endif /* FAT12_H */
