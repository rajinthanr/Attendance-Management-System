/**
 * @file    student_db.c
 * @brief   Level 2 (logic) — binary search over the on-flash student list.
 */
#include "student_db.h"
#include "platform_if.h"
#include "crc.h"

static bool read_id(uint32_t index, uint32_t *out)
{
    return plat_flash_read(NV_STUDENTS_OFFSET + (index * 4u), out, sizeof(*out));
}

bool sdb_load(student_db_t *db)
{
    db->loaded = false;
    db->count = 0u;

    if (!plat_flash_read(NV_CONFIG_OFFSET, &db->cfg, sizeof(db->cfg))) {
        return false;
    }
    if (db->cfg.magic != NV_CONFIG_MAGIC) {
        return false;   /* never provisioned */
    }
    if (db->cfg.student_count == 0u || db->cfg.student_count > NV_STUDENTS_MAX) {
        return false;
    }

    db->count = db->cfg.student_count;
    db->loaded = true;
    return true;
}

bool sdb_verify(const student_db_t *db)
{
    if (!db->loaded) {
        return false;
    }

    uint32_t crc = 0xFFFFFFFFu;
    uint32_t remaining = db->count * 4u;
    uint32_t offset = NV_STUDENTS_OFFSET;
    uint8_t chunk[64];

    /* Chunked so the whole list never has to be resident. */
    while (remaining > 0u) {
        uint32_t n = (remaining > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : remaining;

        if (!plat_flash_read(offset, chunk, n)) {
            return false;
        }
        crc = crc32_ieee_update(crc, chunk, n);
        offset += n;
        remaining -= n;
    }

    return ((crc ^ 0xFFFFFFFFu) == db->cfg.student_crc32);
}

bool sdb_contains(const student_db_t *db, uint32_t id)
{
    if (!db->loaded || db->count == 0u) {
        return false;
    }

    uint32_t low = 0u;
    uint32_t high = db->count;   /* exclusive */

    while (low < high) {
        uint32_t mid = low + ((high - low) / 2u);
        uint32_t value;

        if (!read_id(mid, &value)) {
            return false;
        }
        if (value == id) {
            return true;
        }
        if (value < id) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return false;
}

uint32_t sdb_count(const student_db_t *db)
{
    return db->loaded ? db->count : 0u;
}

uint32_t sdb_device_id(const student_db_t *db)
{
    return db->loaded ? db->cfg.device_id : 0u;
}
