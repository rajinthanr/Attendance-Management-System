/**
 * @file    fat12.c
 * @brief   Level 2 (logic) — FAT12 metadata synthesis.
 */
#include "fat12.h"

/* 8.3 name of the exported file, space padded exactly as FAT stores it. */
static const char k_file_name[11] = { 'A','T','T','E','N','D','C','S','V',' ',' ' };
static const char k_volume_label[11] = { 'A','T','T','E','N','D','A','N','C','E',' ' };

#define ATTR_READ_ONLY   0x01u
#define ATTR_VOLUME_ID   0x08u
#define ATTR_ARCHIVE     0x20u

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void zero(uint8_t *p, uint32_t n)
{
    while (n-- > 0u) {
        *p++ = 0u;
    }
}

void fat12_pack_datetime(const app_datetime_t *dt, uint16_t *date, uint16_t *time)
{
    /* FAT epoch is 1980 and seconds have two-second resolution. */
    uint16_t year = (dt->year >= 1980u) ? (uint16_t)(dt->year - 1980u) : 0u;

    *date = (uint16_t)((year << 9) | ((uint16_t)dt->month << 5) | dt->day);
    *time = (uint16_t)(((uint16_t)dt->hour << 11) |
                       ((uint16_t)dt->minute << 5) |
                       ((uint16_t)dt->second / 2u));
}

void fat12_boot_sector(const fat12_vol_t *vol, uint8_t *s)
{
    zero(s, FAT12_SECTOR_SIZE);

    /* Jump instruction. Never executed here, but hosts sanity-check it. */
    s[0] = 0xEBu; s[1] = 0x3Cu; s[2] = 0x90u;

    /* OEM name. "MSWIN4.1" is the value the FAT spec recommends for maximum
     * compatibility, since some drivers special-case what they find here. */
    const char oem[8] = { 'M','S','W','I','N','4','.','1' };
    uint8_t i;
    for (i = 0u; i < 8u; i++) {
        s[3u + i] = (uint8_t)oem[i];
    }

    wr16(&s[11], FAT12_SECTOR_SIZE);            /* bytes per sector      */
    s[13] = (uint8_t)FAT12_SECTORS_PER_CLUSTER; /* sectors per cluster   */
    wr16(&s[14], FAT12_RESERVED_SECTORS);       /* reserved sectors      */
    s[16] = (uint8_t)FAT12_NUM_FATS;            /* number of FATs        */
    wr16(&s[17], FAT12_ROOT_ENTRIES);           /* root dir entries      */
    wr16(&s[19], FAT12_TOTAL_SECTORS);          /* total sectors (16-bit)*/
    s[21] = 0xF8u;                              /* media: fixed disk     */
    wr16(&s[22], FAT12_SECTORS_PER_FAT);        /* sectors per FAT       */
    wr16(&s[24], 32u);                          /* sectors per track     */
    wr16(&s[26], 8u);                           /* heads                 */
    wr32(&s[28], 0u);                           /* hidden sectors        */
    wr32(&s[32], 0u);                           /* total sectors (32-bit)*/

    s[36] = 0x80u;                              /* drive number          */
    s[37] = 0u;                                 /* reserved              */
    s[38] = 0x29u;                              /* extended boot sig     */
    wr32(&s[39], vol->volume_serial);

    for (i = 0u; i < 11u; i++) {
        s[43u + i] = (uint8_t)k_volume_label[i];
    }

    const char fstype[8] = { 'F','A','T','1','2',' ',' ',' ' };
    for (i = 0u; i < 8u; i++) {
        s[54u + i] = (uint8_t)fstype[i];
    }

    s[510] = 0x55u;
    s[511] = 0xAAu;
}

/**
 * Write one 12-bit FAT entry into @p sector, if any of its bytes fall inside
 * that sector.
 *
 * FAT12 entries straddle byte boundaries: entry n occupies the byte at
 * n*3/2 and part of the next one, so an entry can span two sectors. Handling
 * each of its two bytes independently makes that case fall out for free.
 */
static void put_fat_entry(uint8_t *sector, uint32_t sector_base,
                          uint32_t entry, uint16_t value)
{
    uint32_t offset = (entry * 3u) / 2u;
    uint8_t lo, hi;

    if ((entry & 1u) == 0u) {
        /* Even: low byte is the low 8 bits, high nibble goes to the next byte. */
        lo = (uint8_t)(value & 0xFFu);
        hi = (uint8_t)((value >> 8) & 0x0Fu);
    } else {
        /* Odd: low nibble shares a byte with the previous entry. */
        lo = (uint8_t)((value & 0x0Fu) << 4);
        hi = (uint8_t)((value >> 4) & 0xFFu);
    }

    if (offset >= sector_base && offset < (sector_base + FAT12_SECTOR_SIZE)) {
        /* An odd entry ORs into the byte the previous even entry already
         * wrote, so never overwrite it. */
        sector[offset - sector_base] |= lo;
    }
    offset++;
    if (offset >= sector_base && offset < (sector_base + FAT12_SECTOR_SIZE)) {
        if ((entry & 1u) == 0u) {
            sector[offset - sector_base] |= hi;
        } else {
            sector[offset - sector_base] = hi;
        }
    }
}

void fat12_fat_sector(const fat12_vol_t *vol, uint32_t index, uint8_t *sector)
{
    zero(sector, FAT12_SECTOR_SIZE);

    if (index >= FAT12_SECTORS_PER_FAT) {
        return;
    }

    uint32_t base = index * FAT12_SECTOR_SIZE;

    /* How many clusters the file occupies, rounded up. */
    uint32_t clusters = (vol->file_size + (FAT12_SECTOR_SIZE * FAT12_SECTORS_PER_CLUSTER) - 1u)
                        / (FAT12_SECTOR_SIZE * FAT12_SECTORS_PER_CLUSTER);
    if (clusters > FAT12_DATA_SECTORS) {
        clusters = FAT12_DATA_SECTORS;
    }

    /* Entries 0 and 1 are reserved: media byte in the low 8 bits, then EOC. */
    put_fat_entry(sector, base, 0u, 0x0FF8u);
    put_fat_entry(sector, base, 1u, 0x0FFFu);

    /* Only walk the entries that can touch this sector. Each entry covers
     * 1.5 bytes, so the first one is at (base*2)/3; step back one to catch an
     * entry that began in the previous sector. */
    uint32_t first = (base * 2u) / 3u;
    if (first > 0u) {
        first--;
    }
    uint32_t last = (((base + FAT12_SECTOR_SIZE) * 2u) / 3u) + 1u;

    uint32_t e;
    for (e = first; e <= last; e++) {
        if (e < 2u) {
            continue;
        }
        uint32_t cluster_index = e - 2u;   /* 0-based position in the file */

        if (cluster_index >= clusters) {
            break;   /* beyond the file: leave free (0x000) */
        }

        /* Contiguous chain; the final cluster gets the end-of-chain marker. */
        uint16_t value = (cluster_index == (clusters - 1u))
                       ? 0x0FFFu
                       : (uint16_t)(e + 1u);

        put_fat_entry(sector, base, e, value);
    }
}

/** Fill one 32-byte directory entry. */
static void make_dirent(uint8_t *d, const char name[11], uint8_t attr,
                        uint16_t first_cluster, uint32_t size,
                        uint16_t date, uint16_t time)
{
    uint8_t i;

    zero(d, 32u);
    for (i = 0u; i < 11u; i++) {
        d[i] = (uint8_t)name[i];
    }
    d[11] = attr;
    wr16(&d[16], date);   /* creation date    */
    wr16(&d[14], time);   /* creation time    */
    wr16(&d[18], date);   /* last access date */
    wr16(&d[20], 0u);     /* cluster high (always 0 on FAT12) */
    wr16(&d[22], time);   /* write time       */
    wr16(&d[24], date);   /* write date       */
    wr16(&d[26], first_cluster);
    wr32(&d[28], size);
}

void fat12_root_sector(const fat12_vol_t *vol, uint32_t index, uint8_t *sector)
{
    zero(sector, FAT12_SECTOR_SIZE);

    /* Both entries live in the first root sector; the rest stay all-zero,
     * which FAT reads as "no more entries". */
    if (index != 0u) {
        return;
    }

    make_dirent(&sector[0], k_volume_label, ATTR_VOLUME_ID,
                0u, 0u, vol->fat_date, vol->fat_time);

    make_dirent(&sector[32], k_file_name, ATTR_READ_ONLY | ATTR_ARCHIVE,
                2u, vol->file_size, vol->fat_date, vol->fat_time);
}
