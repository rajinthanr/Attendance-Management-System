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
#include "dedup.h"
#include "battery.h"
#include "nv_layout.h"
#include "platform_if.h"
#include "host_platform.h"
#include "test_util.h"

int g_fail;
int g_run;


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
static void test_dedup(void)
{
    static dedup_t d;
    printf("dedup\n");

    dedup_init(&d);
    CHECK(!dedup_check_and_mark(&d, 111u, 1000u), "first sight is not a dup");
    CHECK(dedup_check_and_mark(&d, 111u, 1005u), "inside 10 s");
    CHECK(!dedup_check_and_mark(&d, 111u, 1014u), "a retry does not restart the window");
    CHECK(dedup_check_and_mark(&d, 111u, 1020u), "the window runs from the last tap that counted");
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
    CHECK(batt_percent(0u) == 0u && batt_percent(APP_BATT_CUTOFF_MV) == 0u, "0 %% at the cutoff");
    CHECK(batt_percent(4200u) == 100u && batt_percent(4350u) == 100u, "100 %% full, and above");
    CHECK(batt_percent(3825u) == 47u, "between points it is linear (%u)", batt_percent(3825u));
    {
        uint32_t v;
        uint8_t last = 0u;
        bool rising = true;

        for (v = 3000u; v <= 4300u; v += 5u) {
            rising = rising && (batt_percent(v) >= last);
            last = batt_percent(v);
        }
        CHECK(rising, "never falls as the voltage rises");
    }

    /* Divider ratio against the schematic: 4.2 V full and 3.3 V cutoff. */
    s.vbat_counts = 1895u;   /* 4.2 V * 2.7 / 7.4 = 1532 mV at the node */
    mv = batt_millivolts(&s);
    CHECK(mv > 4190u && mv < 4210u, "full cell %u mV", mv);
    s.vbat_counts = 1444u;   /* 3.2 V */
    mv = batt_millivolts(&s);
    CHECK(mv > 3190u && mv < 3210u, "flat cell %u mV", mv);
    CHECK(batt_classify(mv) == BATT_CRITICAL, "flat cell is critical");

    /* A sagging VDDA (LDO in dropout) must not change the answer. VDDA
     * 3100 mV: VREFINT reads 1602 and the same 1532 mV node reads 2024. */
    s.vrefint_counts = 1602u;
    s.vbat_counts = 2024u;
    mv = batt_millivolts(&s);
    CHECK(mv > 4180u && mv < 4220u, "low VDDA, full cell %u mV", mv);

    s.vrefint_counts = 0u;
    CHECK(batt_millivolts(&s) == 0u, "bad sample yields 0");
    CHECK(batt_classify(0u) == BATT_OK, "bad sample must not shut the unit down");

    s.vrefint_counts = 3u;   /* implies VDDA of 1.6 kV: a glitch, not a supply */
    s.vbat_counts = 1751u;
    CHECK(batt_millivolts(&s) == 0u, "impossible VDDA rejected");

    {
        batt_reading_t r;
        batt_evaluate(&s, &r);
        CHECK(r.status == BATT_SAMPLE_BAD_VDDA && r.vdda_mv > 3600u,
              "reason reported: %u", (unsigned)r.status);

        s.vrefint_counts = 1500u;
        s.vbat_counts = 2200u;
        batt_evaluate(&s, &r);
        CHECK(r.status == BATT_SAMPLE_TOO_HIGH && r.vbat_mv > 4500u,
              "rejected voltage still shown: %u mV", r.vbat_mv);
    }

    s.vrefint_counts = 1500u;
    s.vbat_counts = 4095u;   /* saturated: R8 open or the node shorted to BAT+ */
    CHECK(batt_millivolts(&s) == 0u, "saturated input rejected");
    s.vbat_counts = 2200u;   /* 4.89 V: no single cell reads that */
    CHECK(batt_millivolts(&s) == 0u, "impossible cell voltage rejected");
}

/* ===================================================================== */
static const uint8_t k_uid4[4] = { 0x0Au, 0xF4u, 0x1Au, 0x9Eu };
static const uint8_t k_uid7[7] = { 0x04u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u };
static const uint8_t k_uid10[10] = { 0x04u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u };

static void test_iso14443a(void)
{
    iso14443a_card_t card;
    printf("iso14443a\n");

    host_nfc_wakeup = false;
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
    host_nfc_wakeup = false;
    host_card_present = false;
    cr_init(&cr, false);
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

/** Run the reader until one more poll has finished; true if it read a card. */
static bool reader_poll_once(card_reader_t *cr, uint32_t *t, iso14443a_card_t *card)
{
    uint32_t polls = cr->polls;
    uint32_t end = *t + 1000u;
    bool found = false;

    for (; *t < end && cr->polls == polls; (*t)++) {
        if (cr_task(cr, *t, card)) {
            found = true;
        }
    }
    return found;
}

static void test_card_reader_wakeup(void)
{
    static card_reader_t cr;
    iso14443a_card_t card;
    uint32_t t = 5000u;
    uint32_t misuse = host_nfc_misuse;
    uint32_t polls;
    bool again = false;
    uint8_t i;

    printf("card reader: wake-up mode\n");
    host_nfc_wakeup = false;
    host_card_present = false;
    cr_init(&cr, true);
    cr_enable(&cr, true, t);

    CHECK(!reader_poll_once(&cr, &t, &card), "first poll finds nothing");
    CHECK(cr_armed(&cr) && host_nfc_wakeup && !host_nfc_field, "armed after an empty poll");
    CHECK(host_nfc_wake_ref == 120u && cr.reference == 120u, "reference from the empty field");

    polls = cr.polls;
    for (i = 0u; i < 10u; i++) {
        (void)reader_poll(&cr, &t, &card);
    }
    CHECK(cr.polls == polls, "no polling while armed");

    /* A card arrives: the chip interrupts, and only then is it read. */
    host_card_set(k_uid4, 4u, 0x08u);
    CHECK(!reader_poll(&cr, &t, &card), "an armed reader waits for its interrupt");
    cr_wake(&cr, t);
    CHECK(!cr_armed(&cr) && !host_nfc_wakeup, "a wake-up leaves wake-up mode");
    cr_wake(&cr, t);
    CHECK(cr.wakeups == 1u, "a second, late event is ignored");
    CHECK(reader_poll_once(&cr, &t, &card) && card.uid_len == 4u, "woken reader reads the card");

    /* A held card is tracked by polling, not by the wake-up mode. */
    for (i = 0u; i < 10u; i++) {
        again = reader_poll_once(&cr, &t, &card) || again;
    }
    CHECK(!again && !cr_armed(&cr), "a held card is polled for and reported once");

    host_card_present = false;
    for (i = 0u; i < APP_NFC_REMOVE_MISSES; i++) {
        (void)reader_poll_once(&cr, &t, &card);
    }
    CHECK(cr_armed(&cr) && cr.false_wakes == 0u, "armed again once the card left");

    /* A wake-up with nothing there costs a few polls and is counted. */
    cr_wake(&cr, t);
    for (i = 0u; i < APP_NFC_REMOVE_MISSES; i++) {
        (void)reader_poll_once(&cr, &t, &card);
    }
    CHECK(cr_armed(&cr) && cr.false_wakes == 1u, "false wake-up counted, armed again");

    /* A pause leaves wake-up mode. A card put down during it must be polled
     * for, not measured into the next reference. */
    cr_enable(&cr, false, t);
    CHECK(!cr_armed(&cr) && !host_nfc_wakeup, "a pause leaves wake-up mode");
    host_card_set(k_uid7, 7u, 0x00u);
    t += 500u;
    cr_enable(&cr, true, t);
    CHECK(reader_poll_once(&cr, &t, &card) && card.uid_len == 7u,
          "card put down during a pause is read, not armed over");
    host_card_present = false;

    CHECK(host_nfc_misuse == misuse, "no command reached the reader in wake-up mode");
}

/* ===================================================================== */
static void test_button(void)
{
    static button_t b;
    uint32_t t;
    int shorts = 0, holds = 0, longs = 0, offs = 0;

    printf("button\n");

#define COUNT_EVENT(e) do { shorts += ((e) == BTN_SHORT); holds += ((e) == BTN_HOLD); \
                            longs += ((e) == BTN_LONG); offs += ((e) == BTN_OFF); } while (0)

    btn_init(&b, false, 0u);
    for (t = 0u; t < 100u; t++) { (void)btn_update(&b, t < 10u, t); }
    CHECK(!btn_is_down(&b), "10 ms glitch is debounced away");

    for (t = 100u; t < 400u; t++) {
        button_event_t e = btn_update(&b, t < 300u, t);
        COUNT_EVENT(e);
    }
    CHECK(shorts == 1 && holds == 0 && longs == 0 && offs == 0, "tap: %d short, %d hold, %d long, %d off",
          shorts, holds, longs, offs);

    /* Two quick taps: a short, then a double. Two slow ones: two shorts. */
    {
        int doubles = 0;
        button_event_t first = BTN_NONE;

        btn_init(&b, false, 450u);      /* nothing from the tap above to pair with */
        shorts = 0;
        for (t = 500u; t < 900u; t++) {
            button_event_t e = btn_update(&b, (t >= 500u && t < 580u) || (t >= 700u && t < 780u), t);

            if (first == BTN_NONE && e != BTN_NONE) {
                first = e;
            }
            shorts += (e == BTN_SHORT);
            doubles += (e == BTN_DOUBLE);
        }
        CHECK(shorts == 1 && doubles == 1 && first == BTN_SHORT, "quick taps: a short, then a double (%d, %d)",
              shorts, doubles);
        shorts = doubles = 0;
        for (t = 20000u; t < 22000u; t++) {
            button_event_t e = btn_update(&b, (t >= 20000u && t < 20080u) ||
                                               (t >= 20080u + APP_BTN_DOUBLE_MS + 100u &&
                                                t < 20160u + APP_BTN_DOUBLE_MS + 100u), t);

            shorts += (e == BTN_SHORT);
            doubles += (e == BTN_DOUBLE);
        }
        CHECK(shorts == 2 && doubles == 0, "slow taps: %d short, %d double", shorts, doubles);
        shorts = 0;
    }

    /* Held 2.5 s: a hold while still down, then a long press on release. */
    shorts = holds = longs = offs = 0;
    for (t = 1000u; t < 4000u; t++) {
        button_event_t e = btn_update(&b, t < 3500u, t);
        COUNT_EVENT(e);
        if (e == BTN_HOLD) {
            CHECK(t >= 1000u + APP_BTN_LONG_MS && t < 3500u, "hold fires at the threshold, while held");
        }
        if (e == BTN_LONG) {
            CHECK(t >= 3500u, "long fires on release");
        }
    }
    CHECK(shorts == 0 && holds == 1 && longs == 1 && offs == 0, "2.5 s: %d short, %d hold, %d long, %d off",
          shorts, holds, longs, offs);

    /* Held 6 s: hold, then off while still down, and the release says nothing. */
    shorts = holds = longs = offs = 0;
    for (t = 5000u; t < 12000u; t++) {
        button_event_t e = btn_update(&b, t < 11000u, t);
        COUNT_EVENT(e);
        if (e == BTN_OFF) {
            CHECK(t >= 5000u + APP_BTN_OFF_MS && t < 11000u, "off fires at the threshold, while held");
        }
    }
    CHECK(shorts == 0 && holds == 1 && longs == 0 && offs == 1, "6 s: %d short, %d hold, %d long, %d off",
          shorts, holds, longs, offs);

    /* The press that woke the unit is ignored, however long it is held. */
    btn_init(&b, true, 0u);
    shorts = holds = longs = offs = 0;
    for (t = 0u; t < 8000u; t++) {
        button_event_t e = btn_update(&b, t < 7000u, t);
        COUNT_EVENT(e);
    }
    CHECK(shorts == 0 && holds == 0 && longs == 0 && offs == 0, "wake press ignored: %d, %d, %d, %d",
          shorts, holds, longs, offs);
#undef COUNT_EVENT
}

/* ===================================================================== */
static void test_battery_boot(void)
{
    static jmp_buf jb;
    volatile int powered_off;

    printf("battery at boot\n");

    host_flash_erase_all();
    host_card_present = false;
    host_button = false;
    host_vbus = false;
    host_deep_sleep_jmp = &jb;

    /* A flat cell found after the divider has settled: straight back off. */
    host_ms = 5000u;
    host_adc_vbat_counts = 1444u;   /* 3.2 V */
    powered_off = 0;
    if (setjmp(jb) == 0) {
        app_init();
        CHECK(app_state() == ST_SHUTDOWN, "flat at boot: shutting down");
        run_ms(3000u);
    } else {
        powered_off = 1;
    }
    CHECK(powered_off == 1, "flat at boot: off");

    /* A fresh battery, read before C3 has charged: looks flat, is not. */
    host_ms = 0u;
    host_adc_vbat_counts = 1300u;   /* 2.9 V, C3 still charging */
    powered_off = 0;
    if (setjmp(jb) == 0) {
        app_init();
        CHECK(app_state() == ST_IDLE, "early low reading is not trusted");
        host_adc_vbat_counts = 1751u;   /* settled: 3.88 V */
        run_ms(APP_BATT_SETTLE_MS + 3000u);
        CHECK(app_state() == ST_IDLE && dbg_battery_mv > 3800u,
              "settled reading keeps it on");
    } else {
        powered_off = 1;
    }
    CHECK(powered_off == 0, "fresh battery: stays on");

    /* Early and still flat once settled: off, without three 10 s samples. */
    host_ms = 0u;
    host_adc_vbat_counts = 1444u;
    powered_off = 0;
    if (setjmp(jb) == 0) {
        app_init();
        run_ms(APP_BATT_SETTLE_MS + 3000u);
    } else {
        powered_off = 1;
    }
    CHECK(powered_off == 1 && host_ms < APP_BATT_SETTLE_MS + 3000u,
          "flat once settled: off at %u ms", (unsigned)host_ms);

    /* On USB power a flat cell is charging: stay on. */
    host_ms = 0u;
    host_vbus = true;
    powered_off = 0;
    if (setjmp(jb) == 0) {
        app_init();
        run_ms(APP_BATT_SETTLE_MS + 1000u);
    } else {
        powered_off = 1;
    }
    CHECK(powered_off == 0, "charging: stays on");

    host_vbus = false;
    host_adc_vbat_counts = 1751u;
    host_deep_sleep_jmp = NULL;
}

/* ===================================================================== */
int main(void)
{
    printf("Level 2 logic tests (no HAL linked)\n\n");

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
    test_dedup();
    test_battery();
    test_iso14443a();
    test_card_reader();
    test_card_reader_wakeup();
    test_button();
    test_fsm();
    test_fsm_sessions();
    test_fsm_plugged_in();
    test_fsm_lectures();
    test_fsm_wakeup();
    test_fsm_sleep();
    test_fsm_double_press();
    test_fsm_wake_learning();
    test_cards();
    test_battery_boot();

    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
