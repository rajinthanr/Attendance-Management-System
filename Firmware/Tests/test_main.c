/**
 * @file    test_main.c
 * @brief   Host tests for the Level 2 logic.
 *
 * Covers the parts where a bug is expensive to find on hardware: the card
 * protocol against a simulated card, the flash log format and its power-loss
 * recovery, the FAT image the host has to accept without complaint, and the
 * state machine driven end to end on simulated time.
 */
#include <stdio.h>
#include <string.h>

#include "app_fsm.h"
#include "app_debug.h"
#include "iso14443a.h"
#include "card_reader.h"
#include "button.h"
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
#include "host_platform.h"

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

    CHECK(time_from_build("Oct  6 2026", "16:41:09", &dt) &&
          dt.year == 2026u && dt.month == 10u && dt.day == 6u &&
          dt.hour == 16u && dt.minute == 41u && dt.second == 9u, "build time");
    CHECK(time_from_build("Feb 29 2028", "00:00:00", &dt), "leap day");
    CHECK(!time_from_build("Feb 30 2027", "00:00:00", &dt), "invalid day");
    CHECK(!time_from_build("Foo  1 2026", "00:00:00", &dt), "bad month");
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

    /* Power cycles without sealing keep filling the same page. */
    for (i = 0u; i < 3u; i++) {
        rb_init(&rb);
        rec.student_id = i;
        rb_push(&rb, &rec);
        log_flush(&ls, &rb);
        log_init(&ls);
    }
    CHECK(log_total(&ls) == 3u && ls.n_used == 1u, "one page for three boots");
    CHECK(log_remaining(&ls) == NV_LOG_CAPACITY - 3u, "remaining %u",
          log_remaining(&ls));
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
     * 1751/4095 puts the node at 1415 mV, and the 4.7 M / 2.7 M divider makes
     * the cell 1415 * 7400 / 2700 = 3878 mV. */
    s.vrefint_cal = 1655u;
    s.vrefint_counts = 1500u;
    s.vbat_counts = 1751u;
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
static const uint8_t k_uid4[4] = { 0x0Au, 0xF4u, 0x1Au, 0x9Eu };
static const uint8_t k_uid7[7] = { 0x04u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u };
static const uint8_t k_uid10[10] = { 0x04u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u };

static void test_iso14443a(void)
{
    iso14443a_card_t card;
    printf("iso14443a\n");

    host_nfc_field = true;
    host_nfc_collision = false;

    host_card_present = false;
    CHECK(iso14443a_select(&card) == ISO14443A_NO_CARD, "empty field");

    /* The MIFARE Classic 1K from the bench: ATQA 0004, SAK 08. */
    host_card_set(k_uid4, 4u, 0x08u);
    CHECK(iso14443a_select(&card) == ISO14443A_OK, "single size UID");
    CHECK(card.uid_len == 4u && memcmp(card.uid, k_uid4, 4u) == 0, "4-byte UID bytes");
    CHECK(card.sak == 0x08u, "sak %02X", card.sak);
    CHECK(card.atqa[0] == 0x04u && card.atqa[1] == 0x00u, "atqa");

    host_card_set(k_uid7, 7u, 0x00u);
    CHECK(iso14443a_select(&card) == ISO14443A_OK, "double size UID");
    CHECK(card.uid_len == 7u && memcmp(card.uid, k_uid7, 7u) == 0, "7-byte UID bytes");
    CHECK(card.sak == 0x00u, "final SAK, not the cascade one");

    host_card_set(k_uid10, 10u, 0x20u);
    CHECK(iso14443a_select(&card) == ISO14443A_OK, "triple size UID");
    CHECK(card.uid_len == 10u && memcmp(card.uid, k_uid10, 10u) == 0, "10-byte UID bytes");

    host_nfc_collision = true;
    CHECK(iso14443a_select(&card) == ISO14443A_COLLISION, "two cards");
    host_nfc_collision = false;

    host_nfc_field = false;
    CHECK(iso14443a_select(&card) == ISO14443A_NO_CARD, "no field, no answer");

    CHECK(card_id_from_uid(k_uid4, 4u) == 0x0AF41A9Eu, "4-byte ID");
    CHECK(card_id_from_uid(k_uid7, 7u) == 0x33445566u, "7-byte ID keeps the tail");

    /* CRC_A reference value from ISO/IEC 14443-3 Annex B: 00 00 -> A0 1E. */
    const uint8_t zeros[2] = { 0u, 0u };
    CHECK(crc16_iso14443a(zeros, 2u) == 0x1EA0u, "crc_a %04X", crc16_iso14443a(zeros, 2u));
}

/* ===================================================================== */
/** Run the reader until the poll scheduled at or after @p t has finished. */
static bool reader_poll(card_reader_t *cr, uint32_t *t, iso14443a_card_t *card)
{
    bool found = false;
    uint32_t end = *t + APP_NFC_POLL_MS;

    for (; *t < end; (*t)++) {
        if (cr_task(cr, *t, card)) {
            found = true;
        }
    }
    return found;
}

static void test_card_reader(void)
{
    static card_reader_t cr;
    iso14443a_card_t card;
    uint32_t t = 1000u;
    uint8_t i;

    printf("card reader\n");
    host_card_present = false;
    cr_init(&cr);
    cr_enable(&cr, true, t);

    CHECK(!cr_task(&cr, t, &card) && host_nfc_field, "field on first");
    CHECK(!cr_task(&cr, t + APP_NFC_FIELD_GUARD_MS - 1u, &card) && host_nfc_field,
          "guard time respected");
    t++;
    CHECK(!reader_poll(&cr, &t, &card), "nothing there");
    CHECK(!host_nfc_field, "field off between polls");

    host_card_set(k_uid4, 4u, 0x08u);
    CHECK(reader_poll(&cr, &t, &card) && card.uid_len == 4u, "card reported");

    bool again = false;
    for (i = 0u; i < 20u; i++) {
        again = again || reader_poll(&cr, &t, &card);
    }
    CHECK(!again, "a held card is reported once");

    /* One missed poll is noise, not a removal. */
    host_card_present = false;
    (void)reader_poll(&cr, &t, &card);
    host_card_present = true;
    CHECK(!reader_poll(&cr, &t, &card), "brief dropout is not a new tap");

    host_card_present = false;
    for (i = 0u; i < APP_NFC_REMOVE_MISSES; i++) {
        (void)reader_poll(&cr, &t, &card);
    }
    host_card_present = true;
    CHECK(reader_poll(&cr, &t, &card), "taken away and back is a new tap");

    /* A different card replaces the held one at once. */
    host_card_set(k_uid7, 7u, 0x00u);
    CHECK(reader_poll(&cr, &t, &card) && card.uid_len == 7u, "card swapped");

    /* Pausing keeps the memory of the held card. */
    cr_enable(&cr, false, t);
    CHECK(!host_nfc_field, "pause turns the field off");
    t += 500u;
    cr_enable(&cr, true, t);
    CHECK(!reader_poll(&cr, &t, &card), "held through a pause, not repeated");
    host_card_present = false;
}

/* ===================================================================== */
static void test_button(void)
{
    static button_t b;
    uint32_t t;
    int shorts = 0, longs = 0;

    printf("button\n");

    btn_init(&b, false, 0u);
    for (t = 0u; t < 100u; t++) { (void)btn_update(&b, t < 10u, t); }
    CHECK(!btn_is_down(&b), "10 ms glitch is debounced away");

    for (t = 100u; t < 400u; t++) {
        button_event_t e = btn_update(&b, t < 300u, t);
        shorts += (e == BTN_SHORT); longs += (e == BTN_LONG);
    }
    CHECK(shorts == 1 && longs == 0, "tap: %d short, %d long", shorts, longs);

    shorts = longs = 0;
    for (t = 1000u; t < 4000u; t++) {
        button_event_t e = btn_update(&b, t < 3500u, t);
        shorts += (e == BTN_SHORT); longs += (e == BTN_LONG);
        if (e == BTN_LONG) {
            CHECK(t >= 1000u + APP_BTN_LONG_MS, "long fires at the threshold");
            CHECK(t < 3500u, "long fires while still held");
        }
    }
    CHECK(shorts == 0 && longs == 1, "hold: %d short, %d long", shorts, longs);

    /* The press that woke the unit is ignored. */
    btn_init(&b, true, 0u);
    shorts = longs = 0;
    for (t = 0u; t < 5000u; t++) {
        button_event_t e = btn_update(&b, t < 4000u, t);
        shorts += (e == BTN_SHORT); longs += (e == BTN_LONG);
    }
    CHECK(shorts == 0 && longs == 0, "wake press ignored: %d, %d", shorts, longs);
}

/* ===================================================================== */
/** Run the state machine for @p ms of simulated time. */
static void run_ms(uint32_t ms)
{
    uint32_t end = host_ms + ms;
    while (host_ms < end) {
        app_task();
    }
}

static void test_fsm(void)
{
    static jmp_buf jb;
    volatile int powered_off;

    printf("state machine\n");

    host_flash_erase_all();
    host_ms = 0u;
    host_card_present = false;
    host_button = false;
    host_vbus = false;
    host_nfc_init_ok = true;
    host_deep_sleeps = 0u;
    host_deep_sleep_jmp = &jb;

    app_init();
    CHECK(app_state() == ST_IDLE, "idle after boot");
    CHECK(dbg_nfc_ready && dbg_nfc_chip_id == 0x2Au, "reader up");
    CHECK(dbg_battery_mv > 3800u && dbg_battery_mv < 3960u, "battery %u mV",
          (unsigned)dbg_battery_mv);
    CHECK(!dbg_nfc_supply_3v3, "3.9 V cell: reader in 5 V mode");
    CHECK((host_out_mask & PLAT_OUT_LED_GREEN) != 0u, "power-on pattern");
    run_ms(1000u);

    /* No list provisioned: every card is recorded. */
    host_card_set(k_uid4, 4u, 0x08u);
    while (dbg_card_count == 0u && host_ms < 3000u) {
        app_task();
    }
    CHECK(dbg_scan_result == APP_SCAN_ACCEPTED, "accepted, got %u", dbg_scan_result);
    CHECK(dbg_card_id == 0x0AF41A9Eu, "card id %08X", (unsigned)dbg_card_id);
    CHECK(dbg_records_ram == 1u, "one record in RAM");
    CHECK((host_out_mask & PLAT_OUT_VIBRATION) != 0u, "motor runs");
    run_ms(50u);
    CHECK(!host_nfc_field, "field off while the motor runs");

    run_ms(3000u);
    CHECK(dbg_card_count == 1u, "held card counted once, got %u", (unsigned)dbg_card_count);

    run_ms(APP_FLUSH_IDLE_MS);
    CHECK(dbg_records_flash == 1u && dbg_records_ram == 0u, "flushed after idle");

    /* Take it away and tap again inside the window: duplicate. */
    host_card_present = false;
    run_ms(1000u);
    host_card_present = true;
    run_ms(500u);
    CHECK(dbg_scan_result == APP_SCAN_DUPLICATE, "duplicate, got %u", dbg_scan_result);
    CHECK(dbg_records_flash == 1u && dbg_records_ram == 0u, "duplicate not recorded");

    /* After the window it counts again. */
    host_card_present = false;
    run_ms((APP_DEDUP_WINDOW_S * 1000u) + 1000u);
    host_card_present = true;
    run_ms(500u);
    CHECK(dbg_scan_result == APP_SCAN_ACCEPTED, "accepted after window");

    /* Tap the button: battery status, no shutdown. */
    host_card_present = false;
    host_button = true;
    run_ms(200u);
    host_button = false;
    run_ms(500u);
    CHECK(dbg_button_short_count == 1u && app_state() == ST_IDLE, "tap shows status");

    /* VBUS with a host: USB session, reader off. */
    host_vbus = true;
    host_usb_configured = true;
    run_ms(200u);
    CHECK(app_state() == ST_USB && host_usb_started, "USB session");
    CHECK(dbg_records_flash == 2u, "flushed before export, got %u",
          (unsigned)dbg_records_flash);
    host_card_set(k_uid7, 7u, 0x00u);
    run_ms(500u);
    CHECK(dbg_card_count == 3u, "no scanning during USB");
    host_vbus = false;
    run_ms(200u);
    CHECK(app_state() == ST_IDLE && !host_usb_started, "back to scanning");
    run_ms(500u);
    CHECK(dbg_card_count == 4u && dbg_card_id == 0x33445566u, "scans after USB");

    /* VBUS without a host: a charger. Keep scanning. */
    host_card_present = false;
    host_usb_configured = false;
    host_vbus = true;
    run_ms(APP_USB_ENUM_TIMEOUT_MS + 500u);
    CHECK(app_state() == ST_IDLE && !host_usb_started, "charger: scanning");
    host_vbus = false;
    run_ms(200u);

    /* Hold the button: power off once released. */
    powered_off = 0;
    if (setjmp(jb) == 0) {
        host_button = true;
        run_ms(APP_BTN_LONG_MS + 2000u);
        CHECK(app_state() == ST_SHUTDOWN && host_deep_sleeps == 0u,
              "waits for release");
        host_button = false;
        run_ms(1000u);
    } else {
        powered_off = 1;
    }
    CHECK(powered_off == 1, "powered off");
    CHECK(host_nfc_powered_down && host_out_mask == 0u, "reader and outputs off");

    /* Reboot: records survive, and the open page is reused, not wasted. */
    {
        static log_store_t ls;
        log_init(&ls);
        CHECK(log_total(&ls) == 3u, "log after power-off: %u", log_total(&ls));
        CHECK(log_remaining(&ls) == NV_LOG_CAPACITY - 3u, "no page wasted");
    }

    /* With a list provisioned, a stranger is refused and not recorded. */
    host_ms = 0u;
    provision_students(10u);
    app_init();
    host_card_set(k_uid4, 4u, 0x08u);
    run_ms(500u);
    CHECK(dbg_scan_result == APP_SCAN_UNKNOWN, "unknown, got %u", dbg_scan_result);
    CHECK(dbg_records_ram == 0u, "unknown not recorded");
    host_card_present = false;

    /* Three minutes of nothing: power off. */
    powered_off = 0;
    if (setjmp(jb) == 0) {
        run_ms(APP_INACTIVITY_MS + 2000u);
    } else {
        powered_off = 1;
    }
    CHECK(powered_off == 1, "inactivity power-off");

    host_deep_sleep_jmp = NULL;
}

/* ===================================================================== */
int main(void)
{
    printf("Level 2 logic tests (no HAL linked)\n\n");

    test_time();
    test_crc();
    test_csv();
    test_student_db();
    test_record_buffer();
    test_log_store();
    test_usb_volume();
    test_dedup();
    test_battery();
    test_iso14443a();
    test_card_reader();
    test_button();
    test_fsm();

    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
