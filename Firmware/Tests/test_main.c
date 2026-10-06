/**
 * @file    test_main.c
 * @brief   Host tests for the Level 2 logic.
 *
 * Covers the parts where a bug is expensive to find on hardware: the decoder,
 * the flash log format and its power-loss recovery, and the FAT image the host
 * has to accept without complaint.
 */
#include "test_util.h"

#include "em4100.h"
#include "timeutil.h"
#include "crc.h"
#include "csv.h"
#include "fat12.h"
#include "usb_storage.h"
#include "log_store.h"
#include "record_buffer.h"
#include "dedup.h"
#include "battery.h"
#include "nv_layout.h"
#include "platform_if.h"

int g_fail;
int g_run;

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
                        seed = (int)(((uint32_t)seed * 1103515245u) + 12345u);   /* unsigned: wraps by definition */
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

void present_card(uint32_t id)
{
    uint8_t bits[64];

    em4100_encode(0x2Au, id, bits);
    host_cap_n = make_edges(bits, 4, 256u, 0, host_cap_buf, EM4100_MAX_EDGES);
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
     * The divider is 4.7 M over 2.7 M (x2.74), so a node reading of
     * 1750/4095 is 1414 mV, which puts the cell at about 3880 mV. */
    s.vrefint_cal = 1655u;
    s.vrefint_counts = 1500u;
    s.vbat_counts = 1750u;
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
    test_device_cfg();
    test_settings_parser();
    test_record_buffer();
    test_log_store();
    test_usb_volume();
    test_usb_settings();
    test_usb_robustness();
    test_sessions();
    test_fsm();
    test_fsm_sessions();
    test_cards();
    test_dedup();
    test_battery();

    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
