/**
 * @file    student_db.h
 * @brief   Level 2 (logic) — enrolled student lookup.
 *
 * The list is searched in place rather than copied to RAM. A 4096-entry list
 * is 16 kB, a quarter of the part's SRAM, and flash is memory mapped: a
 * binary search costs at most twelve reads of four bytes each, which is far
 * cheaper than the copy and leaves the RAM for the record buffer.
 */
#ifndef STUDENT_DB_H
#define STUDENT_DB_H

#include "app_types.h"
#include "nv_layout.h"

typedef struct {
    nv_config_t cfg;    /**< Cached; the config page is small and read once. */
    uint32_t count;     /**< Validated entry count. */
    bool     loaded;    /**< False when the config page is blank or corrupt. */
} student_db_t;

/**
 * Read and validate the config page.
 * @return true when a usable student list is present.
 */
bool sdb_load(student_db_t *db);

/** Verify the stored CRC-32 over the list. Slow (one pass); boot-time only. */
bool sdb_verify(const student_db_t *db);

/** True when @p id is enrolled. Always false when the list failed to load. */
bool sdb_contains(const student_db_t *db, uint32_t id);

uint32_t sdb_count(const student_db_t *db);

/** Device serial from the config page; 0 when unknown. */
uint32_t sdb_device_id(const student_db_t *db);

#endif /* STUDENT_DB_H */
