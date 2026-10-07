/**
 * @file    test_hostfs.c
 * @brief   See test_hostfs.h.
 */
#include <string.h>
#include "test_hostfs.h"
#include "usb_storage.h"

static uint16_t rd16(const uint8_t *p) { return fat12_rd16(p); }
static uint32_t rd32(const uint8_t *p) { return fat12_rd32(p); }

void hf_mount(hostfs_t *h)
{
    uint8_t sec[FAT12_SECTOR_SIZE];
    uint32_t i;

    memset(h, 0, sizeof(*h));
    (void)usbs_read(0u, sec, 1u);
    h->total = rd16(&sec[19]);
    h->clusters = fat12_cluster_count(h->total);
    for (i = 0u; i < FAT12_SECTORS_PER_FAT; i++) {
        (void)usbs_read(FAT12_FAT_START_LBA + i, &h->fat[i * FAT12_SECTOR_SIZE], 1u);
    }
    (void)usbs_read(FAT12_ROOT_START_LBA, h->root, 1u);
}

bool hf_flush_fat(hostfs_t *h)
{
    uint32_t i;

    for (i = 0u; i < FAT12_SECTORS_PER_FAT; i++) {
        if (!usbs_write(FAT12_FAT_START_LBA + i, &h->fat[i * FAT12_SECTOR_SIZE], 1u) ||
            !usbs_write(FAT12_FAT_START_LBA + FAT12_SECTORS_PER_FAT + i,
                        &h->fat[i * FAT12_SECTOR_SIZE], 1u)) {
            return false;
        }
    }
    return true;
}

bool hf_flush_root(hostfs_t *h)
{
    return usbs_write(FAT12_ROOT_START_LBA, h->root, 1u);
}

int hf_find(const hostfs_t *h, const char *name11)
{
    int i;

    for (i = 0; i < (int)FAT12_ROOT_ENTRIES; i++) {
        const uint8_t *d = &h->root[i * 32];

        if (d[0] == 0x00u) {
            break;
        }
        if (d[0] != 0xE5u && (d[11] & FAT12_ATTR_VOLUME_ID) == 0u &&
            memcmp(d, name11, 11u) == 0) {
            return i;
        }
    }
    return -1;
}

uint32_t hf_size(const hostfs_t *h, int entry)  { return rd32(&h->root[entry * 32 + 28]); }
uint32_t hf_first(const hostfs_t *h, int entry) { return rd16(&h->root[entry * 32 + 26]); }

uint32_t hf_chain_len(const hostfs_t *h, uint32_t first)
{
    uint32_t n = 0u;
    uint32_t c = first;

    while (c >= 2u && c < (h->clusters + 2u) && n <= h->clusters) {
        uint16_t next = fat12_get(h->fat, c);

        n++;
        if (next >= 0x0FF8u) {
            return n;
        }
        c = next;
    }
    return 0u;
}

uint32_t hf_free(const hostfs_t *h)
{
    uint32_t c, n = 0u;

    for (c = 2u; c < (h->clusters + 2u); c++) {
        if (fat12_get(h->fat, c) == 0u) {
            n++;
        }
    }
    return n;
}

int32_t hf_read(const hostfs_t *h, const char *name11, uint8_t *out, uint32_t cap)
{
    int e = hf_find(h, name11);

    if (e < 0) {
        return -1;
    }
    return hf_read_chain(h, hf_first(h, e), hf_size(h, e), out, cap);
}

static void sfn_text(const uint8_t *d, char out[13])
{
    uint32_t i, n = 0u;

    for (i = 0u; i < 8u && d[i] != ' '; i++) { out[n++] = (char)d[i]; }
    if (d[8] != ' ') {
        out[n++] = '.';
        for (i = 8u; i < 11u && d[i] != ' '; i++) { out[n++] = (char)d[i]; }
    }
    out[n] = '\0';
}

int hf_list(const hostfs_t *h, const char *dir11, hf_dirent_t *out, uint32_t max)
{
    static const uint8_t k_at[13] = { 1u, 3u, 5u, 7u, 9u, 14u, 16u, 18u, 20u, 22u, 24u, 28u, 30u };
    int e = hf_find(h, dir11);
    uint32_t c, n = 0u, guard = 0u;
    char lfn[64];
    bool have_lfn = false;
    uint8_t lfn_sum = 0u;

    if (e < 0 || (h->root[e * 32 + 11] & FAT12_ATTR_DIRECTORY) == 0u) {
        return -1;
    }
    memset(lfn, 0, sizeof(lfn));
    c = hf_first(h, e);
    while (c >= 2u && c < (h->clusters + 2u) && guard++ <= h->clusters) {
        uint8_t sec[FAT12_SECTOR_SIZE];
        uint32_t i;
        uint16_t next;

        if (!usbs_read(fat12_cluster_lba(c), sec, 1u)) {
            return -1;
        }
        for (i = 0u; i < FAT12_SECTOR_SIZE; i += 32u) {
            const uint8_t *d = &sec[i];

            if (d[0] == 0x00u) {
                return (int)n;
            }
            if (d[0] == 0xE5u) {
                have_lfn = false;
                continue;
            }
            if (d[11] == FAT12_ATTR_LFN) {
                uint32_t ord = d[0] & 0x1Fu, k;

                if ((d[0] & 0x40u) != 0u) {
                    memset(lfn, 0, sizeof(lfn));
                    have_lfn = true;
                    lfn_sum = d[13];
                }
                for (k = 0u; k < 13u; k++) {
                    uint32_t at = ((ord - 1u) * 13u) + k;
                    uint16_t ch = rd16(&d[k_at[k]]);

                    if (ch != 0x0000u && ch != 0xFFFFu && at < (sizeof(lfn) - 1u)) {
                        lfn[at] = (char)ch;
                    }
                }
                have_lfn = have_lfn && (d[13] == lfn_sum);
                continue;
            }
            if (d[0] == '.') {
                have_lfn = false;
                continue;
            }
            if (n < max) {
                hf_dirent_t *o = &out[n];

                sfn_text(d, o->sfn);
                if (have_lfn && fat12_sfn_checksum((const char *)d) == lfn_sum) {
                    memcpy(o->name, lfn, sizeof(o->name));
                } else {
                    memcpy(o->name, o->sfn, sizeof(o->sfn));
                }
                o->attr = d[11];
                o->first = rd16(&d[26]);
                o->size = rd32(&d[28]);
                o->time = rd16(&d[22]);
                o->date = rd16(&d[24]);
            }
            n++;
            have_lfn = false;
        }
        next = fat12_get(h->fat, c);
        if (next >= 0x0FF8u) {
            break;
        }
        c = next;
    }
    return (int)((n < max) ? n : max);
}

int32_t hf_read_chain(const hostfs_t *h, uint32_t first, uint32_t size, uint8_t *out, uint32_t cap)
{
    uint32_t c = first, done = 0u;

    while (done < size && done < cap && c >= 2u && c < (h->clusters + 2u)) {
        uint8_t sec[FAT12_SECTOR_SIZE];
        uint32_t n = size - done;
        uint16_t next;

        if (!usbs_read(fat12_cluster_lba(c), sec, 1u)) {
            return -1;
        }
        if (n > FAT12_SECTOR_SIZE) { n = FAT12_SECTOR_SIZE; }
        if (n > (cap - done)) { n = cap - done; }
        memcpy(&out[done], sec, n);
        done += n;
        next = fat12_get(h->fat, c);
        if (next >= 0x0FF8u) {
            break;
        }
        c = next;
    }
    return (int32_t)done;
}

static void free_chain(hostfs_t *h, uint32_t first)
{
    uint32_t c = first;
    uint32_t guard = 0u;

    while (c >= 2u && c < (h->clusters + 2u) && guard++ <= h->clusters) {
        uint16_t next = fat12_get(h->fat, c);

        fat12_set(h->fat, c, 0u);
        if (next >= 0x0FF8u) {
            break;
        }
        c = next;
    }
}

bool hf_delete(hostfs_t *h, const char *name11)
{
    int e = hf_find(h, name11);

    if (e < 0) {
        return false;
    }
    free_chain(h, hf_first(h, e));
    h->root[e * 32] = 0xE5u;
    return hf_flush_fat(h) && hf_flush_root(h);
}

bool hf_create(hostfs_t *h, const char *name11, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    uint32_t need = (len + FAT12_SECTOR_SIZE - 1u) / FAT12_SECTOR_SIZE;
    uint32_t picked[FAT12_MAX_CLUSTERS];
    uint32_t n = 0u, c, i;
    int slot = -1;
    uint8_t *d;

    if (hf_find(h, name11) >= 0 && !hf_delete(h, name11)) {
        return false;
    }

    /* First fit from the bottom of the volume, as the common drivers do. */
    for (c = 2u; c < (h->clusters + 2u) && n < need; c++) {
        if (fat12_get(h->fat, c) == 0u) {
            picked[n++] = c;
        }
    }
    if (n < need) {
        return false;       /* disk full: the host stops before writing anything */
    }

    for (i = 0u; i < FAT12_ROOT_ENTRIES; i++) {
        if (h->root[i * 32u] == 0x00u || h->root[i * 32u] == 0xE5u) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return false;
    }

    /* Data first, then FAT, then the directory entry. */
    for (i = 0u; i < need; i++) {
        uint8_t sec[FAT12_SECTOR_SIZE];
        uint32_t chunk = len - (i * FAT12_SECTOR_SIZE);

        if (chunk > FAT12_SECTOR_SIZE) { chunk = FAT12_SECTOR_SIZE; }
        memset(sec, 0, sizeof(sec));
        memcpy(sec, &src[i * FAT12_SECTOR_SIZE], chunk);
        if (!usbs_write(fat12_cluster_lba(picked[i]), sec, 1u)) {
            return false;
        }
        fat12_set(h->fat, picked[i], (i + 1u == need) ? FAT12_EOC : (uint16_t)picked[i + 1u]);
    }
    if (!hf_flush_fat(h)) {
        return false;
    }

    d = &h->root[slot * 32];
    fat12_dirent(d, name11, FAT12_ATTR_ARCHIVE, (need > 0u) ? (uint16_t)picked[0] : 0u, len,
                 0x5A21u, 0x7000u);
    return hf_flush_root(h);
}

bool hf_edit_inplace(hostfs_t *h, const char *name11, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    int e = hf_find(h, name11);
    uint32_t c, done = 0u;

    if (e < 0) {
        return false;
    }
    c = hf_first(h, e);
    while (done < len) {
        uint8_t sec[FAT12_SECTOR_SIZE];
        uint32_t chunk = len - done;
        uint16_t next;

        if (c < 2u || c >= (h->clusters + 2u)) {
            return false;       /* grew past the chain: not an in-place edit */
        }
        if (chunk > FAT12_SECTOR_SIZE) { chunk = FAT12_SECTOR_SIZE; }
        memset(sec, 0, sizeof(sec));
        memcpy(sec, &src[done], chunk);
        if (!usbs_write(fat12_cluster_lba(c), sec, 1u)) {
            return false;
        }
        done += chunk;
        next = fat12_get(h->fat, c);
        if (done < len && next >= 0x0FF8u) {
            return false;
        }
        c = next;
    }
    fat12_dirent(&h->root[e * 32], name11, FAT12_ATTR_ARCHIVE, (uint16_t)hf_first(h, e), len,
                 0x5A21u, 0x7000u);
    return hf_flush_root(h);
}
