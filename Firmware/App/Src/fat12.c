/**
 * @file    fat12.c
 * @brief   Level 2 (logic) — FAT12 structure helpers.
 */
#include "fat12.h"

static const char k_volume_label[11] = { 'A','T','T','E','N','D','A','N','C','E',' ' };

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

uint16_t fat12_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t fat12_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void zero(uint8_t *p, uint32_t n)
{
    while (n-- > 0u) {
        *p++ = 0u;
    }
}

uint32_t fat12_cluster_count(uint32_t total_sectors)
{
    return (total_sectors > FAT12_DATA_START_LBA) ? (total_sectors - FAT12_DATA_START_LBA) : 0u;
}

uint32_t fat12_cluster_lba(uint32_t cluster)
{
    return FAT12_DATA_START_LBA + (cluster - FAT12_FIRST_CLUSTER);
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
    wr16(&s[19], (uint16_t)vol->total_sectors); /* total sectors (16-bit)*/
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

uint16_t fat12_get(const uint8_t *fat, uint32_t cluster)
{
    uint32_t o = (cluster * 3u) / 2u;

    if (cluster >= FAT12_FAT_ENTRIES) {
        return FAT12_EOC;
    }
    if ((cluster & 1u) == 0u) {
        return (uint16_t)(fat[o] | ((uint16_t)(fat[o + 1u] & 0x0Fu) << 8));
    }
    return (uint16_t)((fat[o] >> 4) | ((uint16_t)fat[o + 1u] << 4));
}

void fat12_set(uint8_t *fat, uint32_t cluster, uint16_t value)
{
    uint32_t o = (cluster * 3u) / 2u;

    if (cluster >= FAT12_FAT_ENTRIES) {
        return;
    }
    value &= 0x0FFFu;
    if ((cluster & 1u) == 0u) {
        fat[o] = (uint8_t)(value & 0xFFu);
        fat[o + 1u] = (uint8_t)((fat[o + 1u] & 0xF0u) | (value >> 8));
    } else {
        fat[o] = (uint8_t)((fat[o] & 0x0Fu) | ((value & 0x0Fu) << 4));
        fat[o + 1u] = (uint8_t)(value >> 4);
    }
}

void fat12_fat_init(uint8_t *fat)
{
    zero(fat, FAT12_FAT_BYTES);
    fat12_set(fat, 0u, 0x0FF8u);          /* media byte in the low 8 bits */
    fat12_set(fat, 1u, FAT12_EOC);
}

void fat12_chain(uint8_t *fat, uint32_t first, uint32_t count)
{
    uint32_t i;

    for (i = 0u; i < count; i++) {
        uint32_t c = first + i;

        fat12_set(fat, c, (i == (count - 1u)) ? FAT12_EOC : (uint16_t)(c + 1u));
    }
}

void fat12_dirent(uint8_t *d, const char name[11], uint8_t attr,
                  uint16_t first_cluster, uint32_t size,
                  uint16_t date, uint16_t time)
{
    uint8_t i;

    zero(d, 32u);
    for (i = 0u; i < 11u; i++) {
        d[i] = (uint8_t)name[i];
    }
    d[11] = attr;
    wr16(&d[14], time);   /* creation time    */
    wr16(&d[16], date);   /* creation date    */
    wr16(&d[18], date);   /* last access date */
    wr16(&d[20], 0u);     /* cluster high (always 0 on FAT12) */
    wr16(&d[22], time);   /* write time       */
    wr16(&d[24], date);   /* write date       */
    wr16(&d[26], first_cluster);
    wr32(&d[28], size);
}
