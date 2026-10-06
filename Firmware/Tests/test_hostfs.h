/**
 * @file    test_hostfs.h
 * @brief   A minimal FAT12 "host" that talks to the volume through usbs_read()
 *          and usbs_write() only, the way an operating system would.
 *
 * It models the behaviours that matter to the device: first-fit cluster
 * allocation, both FAT copies written, data before metadata, replace-by-delete
 * versus edit-in-place, and "disk full" when the free clusters run out.
 */
#ifndef TEST_HOSTFS_H
#define TEST_HOSTFS_H

#include "app_types.h"
#include "fat12.h"

typedef struct {
    uint8_t  fat[FAT12_FAT_BYTES];
    uint8_t  root[FAT12_SECTOR_SIZE];
    uint32_t total;      /**< Sectors on the volume, from the boot sector. */
    uint32_t clusters;   /**< Data clusters. */
} hostfs_t;

/** 8.3 names in directory form, for the files the device exposes. */
#define HF_SETTINGS  "SETTINGSCSV"
#define HF_ATTEND    "ATTEND  CSV"
#define HF_STATUS    "STATUS  TXT"

/** Read the boot sector, both FATs' first copy and the root directory. */
void hf_mount(hostfs_t *h);

/** Index of the live directory entry called @p name11, or -1. */
int hf_find(const hostfs_t *h, const char *name11);

/** Size and first cluster from a directory entry. */
uint32_t hf_size(const hostfs_t *h, int entry);
uint32_t hf_first(const hostfs_t *h, int entry);

/** Clusters in @p first's chain (0 when it is free or broken). */
uint32_t hf_chain_len(const hostfs_t *h, uint32_t first);

/** Free clusters on the volume. */
uint32_t hf_free(const hostfs_t *h);

/**
 * Read a whole file by following its chain.
 * @return bytes copied (capped at @p cap), or -1 when it does not exist.
 */
int32_t hf_read(const hostfs_t *h, const char *name11, uint8_t *out, uint32_t cap);

/**
 * Create @p name11 with @p len bytes. An existing file of that name is
 * deleted first, the way a "replace" does. Returns false for disk full or a
 * refused write; nothing is changed on the volume in the disk-full case.
 */
bool hf_create(hostfs_t *h, const char *name11, const void *data, uint32_t len);

/** Overwrite the existing file's sectors and set its size; no reallocation. */
bool hf_edit_inplace(hostfs_t *h, const char *name11, const void *data, uint32_t len);

/** Mark @p name11 deleted and free its clusters. */
bool hf_delete(hostfs_t *h, const char *name11);

/** Rewrite just the root directory sector (e.g. after changing a date). */
bool hf_flush_root(hostfs_t *h);

/** Rewrite both FAT copies. */
bool hf_flush_fat(hostfs_t *h);

#endif /* TEST_HOSTFS_H */
