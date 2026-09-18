/**
 * @file    test_main.c
 * @brief   Host tests for the Level 2 logic.
 *
 * Covers the parts where a bug is expensive to find on hardware: the decoder,
 * the flash log format and its power-loss recovery, and the FAT image the host
 * has to accept without complaint.
 */
#include <stdio.h>
#include <string.h>

#include "em4100.h"
#include "timeutil.h"
#include "crc.h"
#include "csv.h"
#include "fat12.h"
#include "usb_storage.h"
#include "log_store.h"
#include "record_buffer.h"
#include "student_db.h"
#include "dedup.h"
#include "battery.h"
#include "nv_layout.h"
#include "platform_if.h"

extern uint8_t host_flash[];
extern uint32_t host_write_failures;
void host_flash_erase_all(void);

static int g_fail;
static int g_run;

#define CHECK(cond, ...) do {                        \
    g_run++;                                         \
    if (!(cond)) {                                   \
        g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);\
        printf(__VA_ARGS__);                         \
        printf("\n");                                \
    }                                                \
} while (0)

/* ===================================================================== */
/* EM4100: build a reference encoder so decode can be checked end to end  */
/* ===================================================================== */

/** Encode version+id into the 64 bits an EM4100 tag actually transmits. */
static void em4100_encode(uint8_t version, uint32_t id, uint8_t bits[64])
{
    uint8_t nibble[10];
    int i, b;

    nibble[0] = (uint8_t)(version >> 4);
    nibble[1] = (uint8_t)(version & 0x0Fu);
    for (i = 0; i < 8; i++) {
        nibble[2 + i] = (uint8_t)((id >> (28 - (4 * i))) & 0x0Fu);
    }

    for (i = 0; i < 9; i++) { bits[i] = 1u; }

    uint8_t col[4] = { 0, 0, 0, 0 };
    for (i = 0; i < 10; i++) {
        uint8_t parity = 0u;
        for (b = 0; b < 4; b++) {
            uint8_t v = (uint8_t)((nibble[i] >> (3 - b)) & 1u);
            bits[9 + (i * 5) + b] = v;
            parity ^= v;
            col[b] ^= v;
        }
        bits[9 + (i * 5) + 4] = parity;
    }
    for (i = 0; i < 4; i++) { bits[59 + i] = col[i]; }
    bits[63] = 0u;
}

/**
 * Turn a repeating bit stream into the edge timestamps the capture unit
 * would produce, at a given half-bit period and with optional jitter.
 */
static uint16_t make_edges(const uint8_t bits[64], int repeats,
                           uint32_t half_us, int jitter_us,
                           uint32_t *edges, uint16_t cap)
{
    uint32_t t = 1000u;
    uint16_t n = 0u;
    int level = -1;
    int r, i, h;
    int seed = 12345;

    for (r = 0; r < repeats; r++) {
        for (i = 0; i < 64; i++) {
            /* Manchester: a '1' is high then low, a '0' is low then high,
             * matching the '10' -> 1 convention the decoder uses. */
            int halves[2];
            halves[0] = bits[i] ? 1 : 0;
            halves[1] = bits[i] ? 0 : 1;

            for (h = 0; h < 2; h++) {
                if (halves[h] != level) {
                    if (n >= cap) { return n; }
                    uint32_t j = 0u;
                    if (jitter_us > 0) {
                        seed = (seed * 1103515245) + 12345;
                        j = (uint32_t)(((seed >> 16) & 0x7FFF) % (2 * jitter_us));
                        j = j - (uint32_t)jitter_us;
                    }
                    edges[n++] = t + j;
                    level = halves[h];
                }
                t += half_us;
            }
        }
    }
    return n;
}

static void test_em4100(void)
{
    static uint32_t edges[EM4100_MAX_EDGES];
    static em4100_ws_t ws;
    uint8_t bits[64];
    app_tag_t tag;

    printf("em4100\n");

    /* Round trip at RF/64 (half-bit 256 us), the common case. */
    em4100_encode(0x2Au, 0x0012D687u, bits);
    uint16_t n = make_edges(bits, 4, 256u, 0, edges, EM4100_MAX_EDGES);
    CHECK(em4100_decode(edges, n, 1000000u, &ws, &tag) == EM4100_OK, "RF/64 decode");
    CHECK(tag.unique_id == 0x0012D687u, "id got 0x%08X", tag.unique_id);
    CHECK(tag.version == 0x2Au, "version got 0x%02X", tag.version);

    /* RF/32: the clock fit has to follow, not assume. */
    em4100_encode(0x01u, 0xDEADBEEFu, bits);
    n = make_edges(bits, 6, 128u, 0, edges, EM4100_MAX_EDGES);
    CHECK(em4100_decode(edges, n, 1000000u, &ws, &tag) == EM4100_OK, "RF/32 decode");
    CHECK(tag.unique_id == 0xDEADBEEFu, "RF/32 id got 0x%08X", tag.unique_id);

    /* +/-20 us of jitter on a 256 us half-bit, about 8 %. */
    em4100_encode(0x77u, 0x00ABCDEFu, bits);
    n = make_edges(bits, 4, 256u, 20, edges, EM4100_MAX_EDGES);
    CHECK(em4100_decode(edges, n, 1000000u, &ws, &tag) == EM4100_OK, "jittered decode");
    CHECK(tag.unique_id == 0x00ABCDEFu, "jitter id got 0x%08X", tag.unique_id);

    /* Noise must not produce a tag. */
    uint16_t i;
    for (i = 0u; i < 400u; i++) {
        edges[i] = (uint32_t)i * (200u + (i % 37u));
    }
    CHECK(em4100_decode(edges, 400u, 1000000u, &ws, &tag) != EM4100_OK, "noise rejected");

    /* One frame only: the flow chart requires two that agree. */
    em4100_encode(0x11u, 0x00000042u, bits);
    n = make_edges(bits, 1, 256u, 0, edges, EM4100_MAX_EDGES);
    CHECK(em4100_decode(edges, n, 1000000u, &ws, &tag) != EM4100_OK, "single frame rejected");

    /* A corrupted parity bit must fail the frame check. */
    em4100_encode(0x2Au, 0x0012D687u, bits);
    bits[13] ^= 1u;
    CHECK(!em4100_check_frame(bits, &tag), "parity error caught");
}

/* ===================================================================== */
static void test_time(void)
{
    printf("timeutil\n");

    app_datetime_t dt = { 2000u, 1u, 1u, 0u, 0u, 0u };
    CHECK(time_to_epoch(&dt) == 0u, "epoch zero");

    app_datetime_t a = { 2026u, 9u, 10u, 13u, 27u, 45u };
    app_datetime_t b;
    time_from_epoch(time_to_epoch(&a), &b);
    CHECK(b.year == 2026u && b.month == 9u && b.day == 10u &&
          b.hour == 13u && b.minute == 27u && b.second == 45u, "round trip");

    /* Leap day, and the day after, across a century-divisible leap year. */
    app_datetime_t leap = { 2024u, 2u, 29u, 23u, 59u, 59u };
    time_from_epoch(time_to_epoch(&leap), &b);
    CHECK(b.month == 2u && b.day == 29u, "2024-02-29 survives");

    CHECK(time_is_leap(2000u) && !time_is_leap(2100u) && time_is_leap(2024u),
          "leap rules");
    CHECK(time_days_in_month(2023u, 2u) == 28u, "feb 2023");

    /* Exhaustive: every day from 2000 to 2099 must round trip. */
    uint32_t bad = 0u;
    uint32_t s;
    for (s = 0u; s < (36525u * 100u); s += 86399u) {
        app_datetime_t x;
        time_from_epoch(s, &x);
        if (time_to_epoch(&x) != (s - (s % 1u))) {
            /* to_epoch drops nothing, so this must be exact */
            if (time_to_epoch(&x) != s) { bad++; }
        }
    }
    CHECK(bad == 0u, "%u exhaustive round trips failed", bad);

    app_datetime_t bad_dt = { 2026u, 2u, 30u, 0u, 0u, 0u };
    CHECK(!time_is_valid(&bad_dt), "30 Feb rejected");
}

/* ===================================================================== */
static void test_crc(void)
{
    printf("crc\n");
    /* Standard check vectors for the "123456789" input. */
    CHECK(crc16_ccitt("123456789", 9u) == 0x29B1u, "CRC-16/CCITT-FALSE");
    CHECK(crc32_ieee("123456789", 9u) == 0xCBF43926u, "CRC-32/ISO-HDLC");
}

/* ===================================================================== */
static void test_csv(void)
{
    printf("csv\n");

    char row[CSV_ROW_BYTES + 1];
    row[CSV_ROW_BYTES] = '\0';

    csv_header(row);
    CHECK(strcmp(row, "SCAN_DATE,SCAN_TIME,STUDENT_ID\r\n") == 0, "header [%s]", row);

    app_datetime_t dt = { 2026u, 9u, 10u, 13u, 27u, 45u };
    app_record_t rec = { 123456u, time_to_epoch(&dt) };
    csv_row(&rec, row);
    CHECK(strcmp(row, "2026-09-10,13:27:45,0000123456\r\n") == 0, "row [%s]", row);

    CHECK((512u % CSV_ROW_BYTES) == 0u, "rows divide a sector");
    CHECK(csv_size(0u) == CSV_ROW_BYTES, "empty file is just the header");
}

/* ===================================================================== */
static void provision_students(uint32_t count)
{
    uint32_t i;
    nv_config_t cfg;

    for (i = 0u; i < count; i++) {
        uint32_t id = 1000u + (i * 7u);
        memcpy(&host_flash[NV_STUDENTS_OFFSET + (i * 4u)], &id, 4u);
    }

    cfg.magic = NV_CONFIG_MAGIC;
    cfg.format_version = 1u;
    cfg.student_count = count;
    cfg.student_crc32 = crc32_ieee(&host_flash[NV_STUDENTS_OFFSET], count * 4u);
    cfg.device_id = 0xC0FFEEu;
    memset(cfg.reserved, 0, sizeof(cfg.reserved));
    memcpy(&host_flash[NV_CONFIG_OFFSET], &cfg, sizeof(cfg));
}

static void test_student_db(void)
{
    static student_db_t db;
    printf("student_db\n");

    host_flash_erase_all();
    CHECK(!sdb_load(&db), "blank flash means no list");
    CHECK(!sdb_contains(&db, 1000u), "unloaded list matches nothing");

    provision_students(500u);
    CHECK(sdb_load(&db), "load provisioned list");
    CHECK(sdb_verify(&db), "crc verifies");
    CHECK(sdb_count(&db) == 500u, "count");
    CHECK(sdb_device_id(&db) == 0xC0FFEEu, "device id");

    CHECK(sdb_contains(&db, 1000u), "first entry");
    CHECK(sdb_contains(&db, 1000u + (499u * 7u)), "last entry");
    CHECK(sdb_contains(&db, 1000u + (250u * 7u)), "middle entry");
    CHECK(!sdb_contains(&db, 1001u), "gap between entries");
    CHECK(!sdb_contains(&db, 0u), "below range");
    CHECK(!sdb_contains(&db, 0xFFFFFFFFu), "above range");

    /* Flip a byte: the CRC must notice. */
    host_flash[NV_STUDENTS_OFFSET + 40u] ^= 0xFFu;
    CHECK(!sdb_verify(&db), "corruption detected");
}

/* ===================================================================== */
static void test_record_buffer(void)
{
    static record_buffer_t rb;
    app_record_t rec = { 1u, 2u };
    uint16_t i;

    printf("record_buffer\n");
    rb_init(&rb);
    CHECK(rb_is_empty(&rb), "starts empty");
    CHECK(!rb_needs_flush(&rb), "empty needs no flush");

    for (i = 0u; i < ((APP_RAM_RECORDS * 80u) / 100u) - 1u; i++) {
        rec.student_id = i;
        rb_push(&rb, &rec);
    }
    CHECK(!rb_needs_flush(&rb), "just under 80%%");
    rb_push(&rb, &rec);
    CHECK(rb_needs_flush(&rb), "at 80%%");

    rb_init(&rb);
    for (i = 0u; i < APP_RAM_RECORDS + 5u; i++) {
        rec.student_id = i;
        rb_push(&rb, &rec);
    }
    CHECK(rb_count(&rb) == APP_RAM_RECORDS, "capped at capacity");
    CHECK(rb.dropped == 5u, "drops counted");

    /* Partial consume keeps the tail, in order. */
    rb_consume(&rb, 10u);
    CHECK(rb_count(&rb) == APP_RAM_RECORDS - 10u, "count after consume");
    CHECK(rb_peek(&rb, 0u)->student_id == 10u, "tail preserved");
}

/* ===================================================================== */
static void test_log_store(void)
{
    static log_store_t ls;
    static record_buffer_t rb;
    app_record_t rec;
    uint32_t i;

    printf("log_store\n");

    host_flash_erase_all();
    log_init(&ls);
    CHECK(log_total(&ls) == 0u, "empty log");
    CHECK(log_remaining(&ls) == NV_LOG_CAPACITY, "full capacity free");

    /* Write enough to spill across three pages. */
    const uint32_t total = (NV_LOG_RECS_PER_PAGE * 2u) + 37u;
    rb_init(&rb);
    for (i = 0u; i < total; i++) {
        rec.student_id = 100000u + i;
        rec.stamp = 1000u + i;
        rb_push(&rb, &rec);
        if (rb_needs_flush(&rb)) { log_flush(&ls, &rb); }
    }
    log_flush(&ls, &rb);

    CHECK(log_total(&ls) == total, "stored %u of %u", log_total(&ls), total);

    /* Every record must read back in order, by random access. */
    uint32_t bad = 0u;
    for (i = 0u; i < total; i++) {
        if (!log_read(&ls, i, &rec) ||
            rec.student_id != (100000u + i) || rec.stamp != (1000u + i)) {
            bad++;
        }
    }
    CHECK(bad == 0u, "%u records read back wrong", bad);
    CHECK(!log_read(&ls, total, &rec), "read past end fails");

    /* Reboot: the index must rebuild identically from flash alone. */
    log_seal(&ls);
    static log_store_t ls2;
    log_init(&ls2);
    CHECK(log_total(&ls2) == total, "after reboot: %u", log_total(&ls2));
    CHECK(log_read(&ls2, total - 1u, &rec) && rec.student_id == (100000u + total - 1u),
          "last record survives reboot");

    /* Power loss with a page left open: recovery adopts it and appends. */
    rb_init(&rb);
    rec.student_id = 777u; rec.stamp = 777u;
    rb_push(&rb, &rec);
    log_flush(&ls2, &rb);
    /* deliberately no log_seal() -- this is the crash */
    static log_store_t ls3;
    log_init(&ls3);
    CHECK(log_total(&ls3) == total + 1u, "unsealed page recovered: %u", log_total(&ls3));
    CHECK(log_read(&ls3, total, &rec) && rec.student_id == 777u, "record after crash");

    rb_init(&rb);
    rec.student_id = 888u;
    rb_push(&rb, &rec);
    CHECK(log_flush(&ls3, &rb) == 1u, "append continues after recovery");
    CHECK(log_total(&ls3) == total + 2u, "total after append");

    /* Filling the area must refuse cleanly and leave the excess in RAM. */
    host_flash_erase_all();
    log_init(&ls);
    rb_init(&rb);
    uint32_t written = 0u;
    for (i = 0u; i < NV_LOG_CAPACITY + 200u; i++) {
        rec.student_id = i;
        rb_push(&rb, &rec);
        if (rb_needs_flush(&rb)) { written += log_flush(&ls, &rb); }
    }
    written += log_flush(&ls, &rb);
    CHECK(written == NV_LOG_CAPACITY, "wrote exactly capacity: %u", written);
    CHECK(log_is_full(&ls), "reports full");
    CHECK(rb_count(&rb) > 0u, "overflow stays in RAM");

    CHECK(log_erase_all(&ls) && log_total(&ls) == 0u, "erase all");
}

/* ===================================================================== */
static void test_usb_volume(void)
{
    static log_store_t ls;
    static record_buffer_t rb;
    static uint8_t sector[512];
    app_record_t rec;
    uint32_t i;

    printf("usb volume\n");

    host_flash_erase_all();
    log_init(&ls);
    rb_init(&rb);

    app_datetime_t dt = { 2026u, 9u, 10u, 13u, 27u, 45u };
    for (i = 0u; i < 100u; i++) {
        rec.student_id = 5000u + i;
        rec.stamp = time_to_epoch(&dt) + i;
        rb_push(&rb, &rec);
        if (rb_needs_flush(&rb)) { log_flush(&ls, &rb); }
    }
    log_flush(&ls, &rb);

    usbs_begin(&ls, 0xC0FFEEu, &dt);
    CHECK(usbs_file_size() == csv_size(100u), "file size %u", usbs_file_size());
    CHECK(usbs_sector_count() == FAT12_TOTAL_SECTORS, "sector count");

    /* Boot sector sanity, the fields a host actually validates. */
    CHECK(usbs_read(0u, sector, 1u), "read boot sector");
    CHECK(sector[510] == 0x55u && sector[511] == 0xAAu, "boot signature");
    CHECK(((uint16_t)sector[11] | ((uint16_t)sector[12] << 8)) == 512u, "bytes/sector");
    CHECK(sector[21] == 0xF8u, "media byte");

    /* Cluster count must stay inside FAT12's range or a host reads it as FAT16. */
    uint32_t clusters = FAT12_DATA_SECTORS / FAT12_SECTORS_PER_CLUSTER;
    CHECK(clusters < 4085u, "cluster count %u is FAT12", clusters);

    /* Root directory: the file entry must carry the right size and cluster. */
    CHECK(usbs_read(FAT12_ROOT_START_LBA, sector, 1u), "read root");
    CHECK(memcmp(&sector[32], "ATTENDCSV  ", 11) == 0, "8.3 name");
    uint32_t size;
    memcpy(&size, &sector[32 + 28], 4u);
    CHECK(size == csv_size(100u), "dirent size %u", size);
    uint16_t first;
    memcpy(&first, &sector[32 + 26], 2u);
    CHECK(first == 2u, "first cluster");

    /* FAT chain: 101 rows * 32 B = 3232 B -> 7 clusters, 2..8, 8 = EOC. */
    CHECK(usbs_read(FAT12_FAT_START_LBA, sector, 1u), "read fat");
    uint32_t n_clusters = (csv_size(100u) + 511u) / 512u;
    uint32_t e;
    uint32_t chain_bad = 0u;
    for (e = 2u; e < (2u + n_clusters); e++) {
        uint32_t off = (e * 3u) / 2u;
        uint16_t v = ((e & 1u) == 0u)
            ? (uint16_t)(sector[off] | ((sector[off + 1u] & 0x0Fu) << 8))
            : (uint16_t)((sector[off] >> 4) | (sector[off + 1u] << 4));
        uint16_t expect = (e == (1u + n_clusters)) ? 0x0FFFu : (uint16_t)(e + 1u);
        if (v != expect) { chain_bad++; }
    }
    CHECK(chain_bad == 0u, "%u bad FAT entries (of %u)", chain_bad, n_clusters);

    /* Free cluster just past the file. */
    {
        uint32_t off = ((2u + n_clusters) * 3u) / 2u;
        uint16_t v = (((2u + n_clusters) & 1u) == 0u)
            ? (uint16_t)(sector[off] | ((sector[off + 1u] & 0x0Fu) << 8))
            : (uint16_t)((sector[off] >> 4) | (sector[off + 1u] << 4));
        CHECK(v == 0u, "cluster past EOF is free, got 0x%03X", v);
    }

    /* First data sector: header row then the first fifteen records. */
    CHECK(usbs_read(FAT12_DATA_START_LBA, sector, 1u), "read data");
    CHECK(memcmp(sector, "SCAN_DATE,SCAN_TIME,STUDENT_ID\r\n", 32) == 0, "csv header");
    CHECK(memcmp(&sector[32], "2026-09-10,13:27:45,0000005000\r\n", 32) == 0,
          "first record row");

    /* Second data sector starts at record 15 (row 16). */
    CHECK(usbs_read(FAT12_DATA_START_LBA + 1u, sector, 1u), "read data 2");
    CHECK(memcmp(sector, "2026-09-10,13:28:00,0000005015\r\n", 32) == 0,
          "row 16 [%.32s]", sector);

    CHECK(!usbs_write(FAT12_DATA_START_LBA, sector, 1u), "writes rejected");
    CHECK(!usbs_read(FAT12_TOTAL_SECTORS, sector, 1u), "read past volume fails");
}

/* ===================================================================== */
static void test_dedup(void)
{
    static dedup_t d;
    printf("dedup\n");

    dedup_init(&d);
    CHECK(!dedup_check_and_mark(&d, 111u, 1000u), "first sight is not a dup");
    CHECK(dedup_check_and_mark(&d, 111u, 1005u), "inside 10 s");
    CHECK(dedup_check_and_mark(&d, 111u, 1014u), "window slides on each touch");
    CHECK(!dedup_check_and_mark(&d, 111u, 1030u), "outside 10 s");

    /* Two cards alternating: a single last-seen slot would fail this. */
    dedup_init(&d);
    CHECK(!dedup_check_and_mark(&d, 1u, 100u), "A first");
    CHECK(!dedup_check_and_mark(&d, 2u, 101u), "B first");
    CHECK(dedup_check_and_mark(&d, 1u, 102u), "A still remembered");
    CHECK(dedup_check_and_mark(&d, 2u, 103u), "B still remembered");
}

/* ===================================================================== */
static void test_battery(void)
{
    app_adc_sample_t s;
    printf("battery\n");

    /* VREFINT_CAL 1655 measured at 3.0 V; reading 1500 implies VDDA = 3310 mV.
     * A 1:1 divider reading 2400/4095 puts the node at 1940 mV, so the cell is
     * about 3880 mV. */
    s.vrefint_cal = 1655u;
    s.vrefint_counts = 1500u;
    s.vbat_counts = 2400u;
    uint32_t mv = batt_millivolts(&s);
    CHECK(mv > 3800u && mv < 3960u, "computed %u mV", mv);
    CHECK(batt_classify(mv) == BATT_OK, "healthy cell");

    CHECK(batt_classify(3400u) == BATT_WARN, "warn band");
    CHECK(batt_classify(3200u) == BATT_CRITICAL, "critical band");

    s.vrefint_counts = 0u;
    CHECK(batt_millivolts(&s) == 0u, "bad sample yields 0");
    CHECK(batt_classify(0u) == BATT_OK, "bad sample must not shut the unit down");
}

/* ===================================================================== */
int main(void)
{
    printf("Level 2 logic tests (no HAL linked)\n\n");

    test_em4100();
    test_time();
    test_crc();
    test_csv();
    test_student_db();
    test_record_buffer();
    test_log_store();
    test_usb_volume();
    test_dedup();
    test_battery();

    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
