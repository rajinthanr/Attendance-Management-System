/**
 * @file    test_fsm.c
 * @brief   The whole device on the host: cards are held to the reader, the USB
 *          cable goes in, the host edits SETTINGS.CSV, the cable comes out and
 *          the unit applies it and carries on scanning. Everything runs through
 *          app_task() on simulated time, the loop the target's main() runs.
 */
#include "test_fsm_util.h"

void test_fsm(void)
{
    static hostfs_t h;
    static char csv[8192];
    static char status[513];
    const app_stats_t *st;
    uint32_t fb;

    printf("fsm end to end\n");

    boot_fresh();
    CHECK(app_state() == ST_IDLE, "idle after init");
    st = app_get_stats();

    /* ---- taps: every card is recorded; with no list none is "unknown" ---- */
    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED, "a card is recorded");
    CHECK((g_fb & FB_GREEN_BIT) != 0u && (g_fb & FB_VIB_BIT) != 0u && (g_fb & FB_RED_BIT) == 0u, "green and a buzz");
    CHECK(tap(1007u, 12u) == APP_SCAN_ACCEPTED, "another card");
    CHECK(tap(1000u, 14u) == APP_SCAN_DUPLICATE, "same card again inside 10 s is a duplicate");
    CHECK(st->scans_accepted == 2u && st->scans_duplicate == 1u, "stats %u/%u", st->scans_accepted, st->scans_duplicate);
    CHECK(tap(777777u, 60u) == APP_SCAN_ACCEPTED, "a card the device has never heard of is recorded all the same");
    CHECK((g_fb & FB_RED_BIT) == 0u, "and with no list it is not called unknown");
    CHECK(tap(1000u, 80u) == APP_SCAN_DUPLICATE, "a card already in this lecture is a duplicate, however late");
    CHECK(tap(777777u, 90u) == APP_SCAN_DUPLICATE, "...whoever it belongs to");

    /* ---- plug in: ATTEND.CSV has card and time, nothing else ---- */
    plug();
    read_attend(&h, csv, sizeof(csv));
    CHECK(strncmp(csv, "DATE,TIME,CARD_ID ", 18u) == 0, "header first");
    CHECK(strstr(csv, "2026-09-10,13:00:10,0000001000\r\n") != NULL, "first tap");
    CHECK(strstr(csv, "2026-09-10,13:00:12,0000001007\r\n") != NULL, "second tap");
    CHECK(strstr(csv, "2026-09-10,13:01:00,0000777777\r\n") != NULL, "third tap");
    CHECK(strstr(csv, "13:00:14") == NULL && strstr(csv, "13:01:20") == NULL, "duplicates are not logged");
    CHECK(rows_in(csv) == 3u && usbs_file_size() == (1u + 3u) * CSV_ROW_BYTES, "3 records + header (%u)", usbs_file_size());
    CHECK(strstr(csv, "UNKNOWN") == NULL && strchr(csv + 40, 'A') == NULL, "the device writes no names");
    read_status(&h, status);
    CHECK(strstr(status, "Last tap     : 0000777777 at 2026-09-10 13:01:00") != NULL, "status shows the last tap [%s]", status);

    /* A card read while the cable is in goes to the PC, not into the log. */
    {
        uint32_t before = dbg_card_count;
        const uint8_t uid[4] = { 0u, 0u, 0x0Fu, 0xA0u };

        host_card_set(uid, 4u, 0x08u);
        run_ms(600u);
        CHECK(dbg_card_count == before + 1u && dbg_scan_result == (uint8_t)APP_SCAN_ENROLLED,
              "read during a USB session, for registering");
        host_card_present = false;
        run_ms(1000u);
        read_attend(&h, csv, sizeof(csv));
        CHECK(rows_in(csv) == 3u && strstr(csv, "0000004000") == NULL, "and not logged");
    }

    /* Start a lecture, in one edit. */
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 2\r\n");
    fb = host_edits_and_unplugs(&h, csv);
    CHECK((fb & FB_GREEN_BIT) != 0u && (fb & FB_RED_BIT) == 0u, "applied: green");

    /* ---- the new lecture is in force, without a restart ---- */
    CHECK(tap(777777u, 100u) == APP_SCAN_ACCEPTED, "the same card counts again in a new lecture");
    CHECK(tap(1000u, 120u) == APP_SCAN_ACCEPTED, "and so does every other");

    plug();
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 5u && usbs_file_size() == (1u + 5u) * CSV_ROW_BYTES, "records accumulate; the lecture marker is not a row (%u)", usbs_file_size());
    CHECK(strstr(csv, "2026-09-10,13:01:40,0000777777\r\n") != NULL, "the new tap");
    read_status(&h, status);
    CHECK(strstr(status, "Lecture      : EN2090 / Lecture 2") != NULL, "the device reports its lecture [%s]", status);

    /* ---- a refused edit: red, and nothing changes ---- */
    hf_mount(&h);
    CHECK(hf_create(&h, HF_SETTINGS, "x", 0u), "host leaves an empty file");
    fb = unplug();
    CHECK((fb & FB_RED_BIT) != 0u && (fb & FB_GREEN_BIT) == 0u, "refused: red");
    plug();
    read_status(&h, status);
    CHECK(strstr(status, "Lecture      : EN2090 / Lecture 2") != NULL, "the lecture is as it was [%s]", status);

    /* ---- the device ID, and a flash write that silently stores the wrong bit ---- */
    host_corrupt_write_after(0u);
    snprintf(csv, sizeof(csv), "#DEVICE,4242\r\n");
    fb = host_edits_and_unplugs(&h, csv);
    host_corrupt_write = 0u;
    CHECK((fb & FB_RED_BIT) != 0u && (fb & FB_GREEN_BIT) == 0u, "an ID that does not read back shows red");

    plug();
    snprintf(csv, sizeof(csv), "#DEVICE,4242\r\n");
    fb = host_edits_and_unplugs(&h, csv);
    CHECK((fb & FB_GREEN_BIT) != 0u, "the same edit without the fault is applied");
    reboot();
    plug();
    hf_mount(&h);
    read_status(&h, status);
    CHECK(strstr(status, "Device ID    : 0000004242") != NULL, "and the device reports it [%s]", status);
    CHECK(usbs_read(0u, (uint8_t *)csv, 1u) && fat12_rd32((const uint8_t *)&csv[39]) == 4242u, "as the volume serial");

    /* ---- plug in, look, unplug: no edit, no flash churn, no pattern ---- */
    {
        uint32_t e0 = host_flash_erases;

        fb = unplug();
        CHECK(fb == 0u, "nothing to report after a look-only session (outputs %u)", fb);
        CHECK(host_flash_erases == e0, "no pages erased by a look-only session");
    }

    /* ---- the clock, set from the USB drive ---- */
    plug();
    snprintf(csv, sizeof(csv), "#TIME,2031-12-31 23:59:00\r\n");
    fb = host_edits_and_unplugs(&h, csv);
    CHECK((fb & FB_GREEN_BIT) != 0u, "the clock edit is applied");
    {
        app_datetime_t now;

        plat_rtc_get(&now);
        CHECK(now.year == 2031u && now.month == 12u && now.day == 31u && now.hour == 23u && now.minute == 59u,
              "the typed time is in the RTC (%04u-%02u-%02u %02u:%02u)", now.year, now.month, now.day, now.hour, now.minute);
    }
    CHECK(tap(555u, 5u) == APP_SCAN_ACCEPTED, "and the unit keeps recording");

    /* ---- power off by the button: nothing recorded is lost ---- */
    power_cycle();
    plug();
    read_attend(&h, csv, sizeof(csv));
    CHECK(strstr(csv, "0000000555") != NULL && strstr(csv, "0000777777") != NULL, "taps survive a power-off");
    (void)unplug();
}

/* ===================================================================== */
/* Lectures and double taps                                               */
/* ===================================================================== */

void test_fsm_sessions(void)
{
    static hostfs_t h;
    static char csv[8192];
    static log_store_t ls;
    const uint32_t hour = 3600u;
    uint32_t fb, total;

    printf("fsm sessions and duplicate taps\n");

    boot_fresh();

    /* A card that stays on the reader, or is read twice in a row. */
    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED, "first tap recorded");
    CHECK(tap(1000u, 11u) == APP_SCAN_DUPLICATE, "second read of the same tap: duplicate");

    /* The student comes back later, after the unit was off (RAM, and so the
     * short-window table, is gone): the flash log remembers. */
    power_cycle();
    CHECK(tap(1000u, hour) == APP_SCAN_DUPLICATE, "a tap after power-off is still a duplicate");
    CHECK(tap(1007u, hour + 100u) == APP_SCAN_ACCEPTED, "a different card is not");

    /* A different sitting, hours later with no lecture started. */
    CHECK(tap(1000u, 7u * hour) == APP_SCAN_ACCEPTED, "beyond APP_SESSION_MAX_AGE_S it counts again");

    /* A lecture is started from the USB drive. */
    plug();
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n");
    fb = host_edits_and_unplugs(&h, csv);
    CHECK((fb & FB_GREEN_BIT) != 0u, "lecture started: applied");
    CHECK(tap(1000u, 7u * hour + 100u) == APP_SCAN_ACCEPTED, "the new lecture starts everyone afresh");
    CHECK(tap(1000u, 7u * hour + 200u) == APP_SCAN_DUPLICATE, "and a second tap in it is a duplicate");
    CHECK(tap(1007u, 7u * hour + 210u) == APP_SCAN_ACCEPTED, "other cards sign in");

    /* An edit that leaves the names alone does not start a lecture. */
    plug();                              /* the attach flushes the RAM buffer to flash */
    log_init(&ls);
    total = log_total(&ls);
    hf_mount(&h);
    {
        static char same[4096];
        int32_t n = hf_read(&h, HF_SETTINGS, (uint8_t *)same, sizeof(same) - 1u);

        CHECK(n > 0, "the file can be read");
        same[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(same, "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n") != NULL, "the file shows the lecture in progress");
        CHECK(hf_edit_inplace(&h, HF_SETTINGS, same, (uint32_t)n), "host saves the file unchanged");
    }
    (void)unplug();
    log_init(&ls);
    CHECK(log_total(&ls) == total, "an unchanged edit adds nothing to the log (%u vs %u)", log_total(&ls), total);
    CHECK(tap(1000u, 7u * hour + 300u) == APP_SCAN_DUPLICATE, "so the lecture carries on");

    /* The same names again, as a second lecture: #NEWSESSION. */
    plug();
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n#NEWSESSION,1\r\n");
    fb = host_edits_and_unplugs(&h, csv);
    CHECK((fb & FB_GREEN_BIT) != 0u, "second lecture with the same names: applied");
    CHECK(tap(1000u, 7u * hour + 400u) == APP_SCAN_ACCEPTED, "#NEWSESSION lets everyone sign in again");

    /* ATTEND.CSV: rows only, markers invisible. */
    plug();
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 6u, "6 attendance rows, no marker rows (%u)", rows_in(csv));
    CHECK(strstr(csv, "4294967") == NULL, "no marker id leaks into the file");
    CHECK(strstr(csv, "EN2090") == NULL, "no lecture name either: the PC keeps the lectures");
    read_status(&h, csv);
    CHECK(strstr(csv, "Lecture      : EN2090 / Lecture 1") != NULL, "the device still knows which lecture is running");
    (void)unplug();
}

/* ===================================================================== */
/* Still plugged in: eject, the button, clearing the log                  */
/* ===================================================================== */

/** Run until the USB session has ended; returns the first feedback outputs. */
static uint32_t until_scanning(void)
{
    uint32_t i;

    for (i = 0u; i < (APP_USB_EJECT_GRACE_MS + 500u) && app_state() == ST_USB; i++) {
        app_task();
    }
    CHECK(app_state() == ST_IDLE && !host_usb_started, "the drive is gone and the unit scans");
    g_fb = ((host_out_mask & FB_VIB_BIT) != 0u) ? host_out_mask : 0u;
    run_ms(1500u);
    return g_fb;
}

static void cable_out_and_in(void)
{
    host_vbus = false;
    run_ms(300u);
    host_usb_ejected = false;
    plug();
}

void test_fsm_plugged_in(void)
{
    static hostfs_t h;
    static char csv[8192];
    static char status[513];
    uint32_t fb;

    printf("fsm while plugged in\n");

    boot_fresh();
    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED, "a tap from the last lecture");
    CHECK(tap(1007u, 12u) == APP_SCAN_ACCEPTED, "and another");

    /* ---- the app starts a lecture, clears the log and ejects ---- */
    plug();
    hf_mount(&h);
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 5\r\n#NEWSESSION,1\r\n#CLEARLOG,1\r\n");
    CHECK(hf_create(&h, HF_SETTINGS, csv, (uint32_t)strlen(csv)), "host copies SETTINGS.CSV");
    read_status(&h, status);
    CHECK(strstr(status, "all records will be deleted") != NULL, "STATUS.TXT warns of the clear [%s]", status);
    CHECK(strstr(status, "a new lecture will start") != NULL, "and announces the lecture");

    run_ms(500u);
    CHECK(app_state() == ST_USB, "nothing happens before the eject");
    host_usb_ejected = true;
    run_ms(APP_USB_EJECT_GRACE_MS / 2u);
    CHECK(app_state() == ST_USB && host_usb_started, "the host gets time to finish its eject");
    fb = until_scanning();
    CHECK((fb & FB_GREEN_BIT) != 0u && (fb & FB_RED_BIT) == 0u, "applied: green");
    CHECK(host_vbus, "still on USB power");

    CHECK(tap(1000u, 100u) == APP_SCAN_ACCEPTED, "the reader works with the cable in");
    run_ms(10000u);
    CHECK(app_state() == ST_IDLE, "the drive does not come back while the cable stays in");
    run_ms(APP_INACTIVITY_MS + 2000u);
    CHECK(app_state() == ST_IDLE, "and on USB power the unit does not switch itself off");
    CHECK(tap(1000u, 400u) == APP_SCAN_DUPLICATE, "the lecture is in force");

    cable_out_and_in();
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 1u, "the old records are gone, the new tap is there (%u rows)", rows_in(csv));
    CHECK(strstr(csv, "2026-09-10,13:01:40,0000001000\r\n") != NULL, "the tap after the eject");
    read_status(&h, status);
    CHECK(strstr(status, "Lecture      : EN2090 / Lecture 5") != NULL, "the marker survived the clear [%s]", status);

    /* ---- a short press does what the eject does ---- */
    hf_mount(&h);
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 6\r\n");
    CHECK(hf_create(&h, HF_SETTINGS, csv, (uint32_t)strlen(csv)), "host copies SETTINGS.CSV");
    host_button = true;
    run_ms(150u);
    host_button = false;
    fb = until_scanning();
    CHECK((fb & FB_GREEN_BIT) != 0u && (fb & FB_RED_BIT) == 0u, "button: applied, green");
    CHECK(tap(1000u, 500u) == APP_SCAN_ACCEPTED, "the new lecture counts the card again");

    /* ---- a press with nothing edited still starts the reader ---- */
    cable_out_and_in();
    host_button = true;
    run_ms(150u);
    host_button = false;
    (void)until_scanning();
    CHECK(tap(1007u, 600u) == APP_SCAN_ACCEPTED, "scanning after a look-only session");

    /* ---- switching off while plugged in keeps the edit ---- */
    cable_out_and_in();
    hf_mount(&h);
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 7\r\n");
    CHECK(hf_create(&h, HF_SETTINGS, csv, (uint32_t)strlen(csv)), "host copies SETTINGS.CSV");
    {
        static jmp_buf jb;
        volatile int off = 0;

        host_deep_sleeps = 0u;
        host_deep_sleep_jmp = &jb;
        if (setjmp(jb) == 0) {
            host_button = true;
            run_ms(APP_BTN_OFF_MS + 1000u);
            host_button = false;
            run_ms(5000u);
        } else {
            off = 1;
        }
        host_deep_sleep_jmp = NULL;
        CHECK(off == 1, "the long press switches the unit off");
    }
    host_vbus = false;
    reboot();
    plug();
    hf_mount(&h);
    read_status(&h, status);
    CHECK(strstr(status, "Lecture      : EN2090 / Lecture 7") != NULL, "the edit was applied before power-off [%s]", status);
    (void)unplug();
}

/* ===================================================================== */
/* Lectures started from the button                                       */
/* ===================================================================== */

/** What the outputs did while the button was held for @p ms and let go. */
typedef struct {
    bool vib_while_held;    /**< The motor ran at some point during the hold. */
    uint32_t after_release; /**< Outputs straight after the release. */
} press_t;

static press_t press(uint32_t ms)
{
    press_t p = { false, 0u };
    uint32_t i;

    host_button = true;
    for (i = 0u; i < ms; i++) {
        run_ms(1u);
        if ((host_out_mask & FB_VIB_BIT) != 0u) {
            p.vib_while_held = true;
        }
    }
    host_button = false;
    run_ms(APP_BTN_DEBOUNCE_MS + 5u);
    p.after_release = host_out_mask;
    run_ms(1500u);
    return p;
}

/** LECTURES.CSV through the volume (already plugged in), NUL terminated. */
static uint32_t read_lectures(hostfs_t *h, char *out, uint32_t cap)
{
    int32_t n;

    hf_mount(h);
    n = hf_read(h, HF_LECTURES, (uint8_t *)out, cap - 1u);
    out[(n < 0) ? 0 : n] = '\0';
    return (n < 0) ? 0u : ((uint32_t)n / CSV_LECTURE_ROW_BYTES) - 1u;
}

void test_fsm_lectures(void)
{
    static hostfs_t h;
    static char csv[8192];
    press_t p;

    printf("fsm lectures from the button\n");

    /* ---- no lecture named yet: "Lecture 1", no module ---- */
    boot_fresh();
    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED, "a tap before any lecture");
    p = press(APP_BTN_LONG_MS + 300u);
    CHECK(p.vib_while_held, "a buzz while held says: let go now for a new lecture");
    CHECK((p.after_release & (FB_GREEN_BIT | FB_VIB_BIT)) == (FB_GREEN_BIT | FB_VIB_BIT),
          "green with the motor on release: lecture started (outputs %u)", p.after_release);
    CHECK(app_state() == ST_IDLE, "and the unit keeps scanning");
    CHECK(tap(1000u, 20u) == APP_SCAN_ACCEPTED, "the same card signs in to the new lecture");

    /* The last card of one lecture, tapped again straight after a new one is
     * started from the button: inside the 10 s repeat window, but a new
     * lecture, so it counts. */
    CHECK(tap(1007u, 30u) == APP_SCAN_ACCEPTED, "the last card of lecture 1");
    p = press(APP_BTN_LONG_MS + 300u);
    CHECK((p.after_release & FB_GREEN_BIT) != 0u, "lecture 2 started a few seconds later");
    CHECK(tap(1007u, 34u) == APP_SCAN_ACCEPTED, "that card signs in to lecture 2 at once");
    CHECK(tap(1007u, 36u) == APP_SCAN_DUPLICATE, "and only once");
    plug();
    CHECK(read_lectures(&h, csv, sizeof(csv)) == 2u, "two lectures on LECTURES.CSV");
    CHECK(strstr(csv, ",,Lecture 1 ") != NULL, "named Lecture 1, no module [%.60s]", &csv[CSV_LECTURE_ROW_BYTES]);
    CHECK(strstr(csv, ",,Lecture 2 ") != NULL, "then Lecture 2");

    /* ---- a lecture named by the PC, then two more from the button ---- */
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Circuits Lecture 4\r\n");
    (void)host_edits_and_unplugs(&h, csv);
    CHECK(tap(1000u, 100u) == APP_SCAN_ACCEPTED, "lecture 4: signed in");
    CHECK(tap(1007u, 110u) == APP_SCAN_ACCEPTED, "lecture 4: another student");
    CHECK(tap(1000u, 120u) == APP_SCAN_DUPLICATE, "lecture 4: a second tap is a duplicate");

    host_set_time(&(app_datetime_t){ 2026u, 9u, 10u, 14u, 0u, 0u });
    p = press(APP_BTN_LONG_MS + 1000u);
    CHECK((p.after_release & FB_GREEN_BIT) != 0u, "lecture 5 started");
    CHECK(tap(1000u, 3700u) == APP_SCAN_ACCEPTED, "lecture 5: everyone signs in afresh");
    CHECK(tap(1000u, 3710u) == APP_SCAN_DUPLICATE, "lecture 5: but only once");

    host_set_time(&(app_datetime_t){ 2026u, 9u, 10u, 15u, 0u, 0u });
    (void)press(APP_BTN_OFF_MS - 500u);     /* just short of switching off */
    CHECK(app_state() == ST_IDLE, "a 4.5 s hold is still a lecture, not a power-off");
    CHECK(tap(1000u, 7300u) == APP_SCAN_ACCEPTED, "lecture 6: signed in");

    /* ---- held to 5 s: off, and no lecture (five, not six, below) ---- */
    power_cycle();

    /* ---- what the PC sees ---- */
    plug();
    CHECK(read_lectures(&h, csv, sizeof(csv)) == 5u, "five lectures in all");
    CHECK(strstr(csv, ",EN2090,Circuits Lecture 4 ") != NULL, "lecture 4 as the PC named it");
    /* Stamped on release, so a few seconds after the hold began. */
    CHECK(strstr(csv, "2026-09-10,14:00:03,EN2090,Circuits Lecture 5 ") != NULL, "lecture 5, numbered on");
    CHECK(strstr(csv, "2026-09-10,15:00:04,EN2090,Circuits Lecture 6 ") != NULL, "lecture 6, numbered on");
    read_status(&h, csv);
    CHECK(strstr(csv, "Lecture      : EN2090 / Circuits Lecture 6") != NULL, "STATUS.TXT shows the newest");
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 8u, "ATTEND.CSV: the 8 recorded taps, no marker rows (%u)", rows_in(csv));

    /* ---- plugged in, the drive is up: a long press only ends the drive session ---- */
    p = press(APP_BTN_LONG_MS + 300u);
    CHECK(app_state() == ST_IDLE && !host_usb_started, "the drive session ended");
    host_vbus = false;
    run_ms(300u);
    plug();
    CHECK(read_lectures(&h, csv, sizeof(csv)) == 5u, "no lecture was started from the drive session");
    (void)unplug();
}

/** The reader sleeps in its wake-up mode between cards and leaves it for
 *  everything the chip cannot do there. */
void test_fsm_wakeup(void)
{
    uint32_t misuse = host_nfc_misuse;
    uint32_t wakeups;
    bool left = false;
    uint32_t i;

    printf("fsm reader wake-up\n");

    boot_fresh();
    CHECK(dbg_nfc_armed && host_nfc_wakeup, "an idle reader waits in wake-up mode");
    wakeups = dbg_nfc_wakeups;
    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED, "a card wakes the reader and is read");
    CHECK(dbg_nfc_wakeups == wakeups + 1u, "through one wake-up (%u)", (unsigned)dbg_nfc_wakeups);
    CHECK(dbg_nfc_armed && host_nfc_wakeup, "armed again once the card has gone");
    CHECK(dbg_nfc_false_wakes == 0u, "no false wake-ups");

    /* A button tap plays a pattern, and the reader pauses for patterns. */
    host_button = true;
    run_ms(150u);
    host_button = false;
    for (i = 0u; i < 500u; i++) {
        app_task();
        left = left || !host_nfc_wakeup;
    }
    CHECK(left, "out of wake-up mode while a pattern plays");
    run_ms(2000u);
    CHECK(host_nfc_wakeup, "and back in afterwards");

    /* Under 3.5 V the reader changes supply mode, which needs Ready mode. */
    host_adc_vbat_counts = 1560u;   /* about 3.46 V */
    run_ms(APP_BATT_SAMPLE_MS + 500u);
    CHECK(dbg_nfc_supply_3v3 && dbg_nfc_ready, "3.3 V supply mode, reader still up");
    CHECK(host_nfc_wakeup, "armed again after the change");
    host_adc_vbat_counts = 1751u;
    run_ms(APP_BATT_SAMPLE_MS + 500u);
    CHECK(!dbg_nfc_supply_3v3 && host_nfc_wakeup, "and back");

    /* The reader keeps running with the drive up, for registering cards. */
    plug();
    run_ms(500u);
    CHECK(host_nfc_wakeup && dbg_nfc_armed, "armed with the drive up too");
    (void)unplug();
    run_ms(1500u);
    CHECK(host_nfc_wakeup, "armed again once the cable is out");
    CHECK(tap(1007u, 100u) == APP_SCAN_ACCEPTED, "and still reading cards");

    CHECK(host_nfc_misuse == misuse, "no command sent to the reader in wake-up mode (%u)",
          (unsigned)(host_nfc_misuse - misuse));
}

/** Between events the loop asks to sleep until its next deadline, deeply. */
void test_fsm_sleep(void)
{
    uint32_t i, longest = 0u;
    bool deep = true;

    printf("fsm sleep\n");

    boot_fresh();
    run_ms(5000u);   /* the power-on pattern and the first battery sample are over */
    for (i = 0u; i < 4000u; i++) {
        app_task();
        deep = deep && host_sleep_deep;
        if ((host_sleep_wake - host_ms) > longest) {
            longest = host_sleep_wake - host_ms;
        }
    }
    CHECK(deep, "idle with the reader armed: Stop 2 allowed throughout");
    /* The pass's own millisecond is already gone when it is measured here. */
    CHECK(longest >= APP_SLEEP_MAX_MS - 1u && longest < APP_SLEEP_MAX_MS, "long sleeps between heartbeats (%u ms)",
          longest);
    CHECK(dbg_nfc_armed, "the reader waits for its interrupt meanwhile");

    /* A pattern plays: wake for each step of it. */
    host_button = true;
    run_ms(100u);
    CHECK((int32_t)(host_sleep_wake - host_ms) <= (int32_t)APP_BTN_LONG_MS,
          "a held button wakes the loop by its hold threshold");
    host_button = false;
    run_ms(APP_BTN_DEBOUNCE_MS + 5u);
    CHECK((int32_t)(host_sleep_wake - host_ms) < 500, "a pattern's steps are short (%d ms)",
          (int)(host_sleep_wake - host_ms));
    run_ms(3000u);

    /* A card held: the poll runs on its own 100 ms deadlines. */
    {
        const uint8_t uid[4] = { 0u, 0u, 0x03u, 0xE8u };

        host_card_set(uid, 4u, 0x08u);
        run_ms(50u);
        CHECK((int32_t)(host_sleep_wake - host_ms) <= (int32_t)APP_NFC_POLL_MS, "polling while a card is there");
        host_card_present = false;
        run_ms(3000u);
    }

    /* Plugged in: no Stop 2, the loop runs every millisecond for USB. */
    plug();
    app_task();
    CHECK(!host_sleep_deep && host_sleep_wake == host_ms, "USB: a millisecond at a time, no Stop 2");
    (void)unplug();
    run_ms(3000u);
    app_task();
    CHECK(host_sleep_deep, "deep again once unplugged");
}

/** Ejected with the cable in: a double press brings the drive back. */
void test_fsm_double_press(void)
{
    printf("fsm double press\n");

    boot_fresh();
    plug();
    host_usb_ejected = true;
    (void)until_scanning();
    CHECK(host_vbus && app_state() == ST_IDLE, "ejected, cable in, taking attendance");

    /* One tap only shows the battery. */
    host_button = true;  run_ms(80u);
    host_button = false; run_ms(1500u);
    CHECK(app_state() == ST_IDLE && !host_usb_started, "a single tap leaves the drive away");

    /* Two quick taps: the drive comes back. */
    host_button = true;  run_ms(80u);
    host_button = false; run_ms(150u);
    host_button = true;  run_ms(80u);
    host_button = false; run_ms(300u);
    CHECK(app_state() == ST_USB && host_usb_started, "a double press brings the drive back");
    (void)unplug();

    /* On battery a double press is just two battery displays. */
    host_button = true;  run_ms(80u);
    host_button = false; run_ms(150u);
    host_button = true;  run_ms(80u);
    host_button = false; run_ms(300u);
    CHECK(app_state() == ST_IDLE && !host_usb_started, "no cable: nothing to bring back");
}

/**
 * The board's case: the chip's wake-up measurement reads 8 counts above
 * Measure amplitude, so a reference taken with the command wakes the reader
 * at once, every time. It must learn the offset, then widen the window only
 * for noise, and still wake for a card.
 */
void test_fsm_wake_learning(void)
{
    uint32_t f0, i;

    printf("fsm wake-up learning\n");

    host_nfc_wu_offset = 8;
    boot_fresh();
    run_ms(3000u);
    f0 = dbg_nfc_false_wakes;
    CHECK(f0 >= 1u && f0 <= 2u, "one false wake-up to learn from (%u)", (unsigned)f0);
    CHECK(dbg_nfc_wake_offset == 8 && dbg_nfc_wake_delta == APP_NFC_WAKE_DELTA_MIN,
          "offset learned (%d), window untouched (%u)", (int)dbg_nfc_wake_offset, dbg_nfc_wake_delta);
    run_ms(20000u);
    CHECK(dbg_nfc_false_wakes == f0 && dbg_nfc_armed, "then it stays asleep (%u false)",
          (unsigned)dbg_nfc_false_wakes);
    CHECK(tap(1000u, 60u) == APP_SCAN_ACCEPTED, "and a card still wakes it");

    /* Noise: the reading wanders by 4 counts. The window widens until it
     * no longer wakes for that, and no further. */
    for (i = 0u; i < 40u; i++) {
        host_nfc_wu_offset = ((i & 1u) != 0u) ? 12 : 8;
        run_ms(500u);
    }
    CHECK(dbg_nfc_wake_delta >= 4u && dbg_nfc_wake_delta <= 5u, "window widened to the noise (%u)",
          dbg_nfc_wake_delta);
    f0 = dbg_nfc_false_wakes;
    for (i = 0u; i < 20u; i++) {
        host_nfc_wu_offset = ((i & 1u) != 0u) ? 12 : 8;
        run_ms(500u);
    }
    CHECK(dbg_nfc_false_wakes <= f0 + 1u, "and then the noise no longer wakes it (%u more)",
          (unsigned)(dbg_nfc_false_wakes - f0));
    CHECK(tap(1007u, 120u) == APP_SCAN_ACCEPTED, "a card still does");
    host_nfc_wu_offset = 0;
}
