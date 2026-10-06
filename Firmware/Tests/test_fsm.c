/**
 * @file    test_fsm.c
 * @brief   The whole device on the host: cards are tapped, the USB cable goes
 *          in, the host edits SETTINGS.CSV, the cable comes out, and the unit
 *          reboots with the new settings. Everything runs through
 *          app_dispatch(), the same entry point the target's main loop uses.
 */
#include "test_util.h"
#include "test_hostfs.h"

#include "app_fsm.h"
#include "app_events.h"
#include "app_config.h"
#include "csv.h"
#include "device_cfg.h"
#include "log_store.h"
#include "usb_storage.h"
#include "platform_if.h"
#include "timeutil.h"

#define OK_FB   (PLAT_OUT_LED_GREEN | PLAT_OUT_VIBRATION)

static app_datetime_t at(uint32_t second_offset)
{
    app_datetime_t dt = { 2026u, 9u, 10u, 13u, 0u, 0u };

    dt.hour = (uint8_t)(13u + (second_offset / 3600u));
    dt.minute = (uint8_t)((second_offset % 3600u) / 60u);
    dt.second = (uint8_t)(second_offset % 60u);
    return dt;
}

/** Step a feedback pattern to its end. */
static void pump_feedback(void)
{
    int i;

    for (i = 0; i < 40 && app_state() == ST_FEEDBACK; i++) {
        app_dispatch(APP_EVT_TIMER);
    }
}

/** Present a card at second @p t; returns the outputs driven for feedback. */
static uint32_t tap(uint32_t id, uint32_t t)
{
    app_datetime_t dt = at(t);
    uint32_t fb;

    host_set_time(&dt);
    app_dispatch(APP_EVT_TOUCH);
    CHECK(app_state() == ST_READING, "touch starts a read");
    present_card(id);
    app_dispatch(APP_EVT_CAPTURE_FULL);
    fb = host_out_mask;
    pump_feedback();
    CHECK(app_state() == ST_IDLE, "back to idle after feedback");
    CHECK(host_out_mask == 0u, "outputs off after feedback");
    return fb;
}

/** Read ATTEND.CSV through the volume into @p out (NUL terminated). */
static void read_attend(hostfs_t *h, char *out, uint32_t cap)
{
    int32_t n;

    hf_mount(h);
    n = hf_read(h, HF_ATTEND, (uint8_t *)out, cap - 1u);
    out[(n < 0) ? 0 : n] = '\0';
}

static uint32_t rows_in(const char *csv)
{
    uint32_t lines = 0u, i;

    for (i = 0u; csv[i] != '\0'; i++) {
        lines += (csv[i] == '\n');
    }
    return (lines > 0u) ? (lines - 1u) : 0u;      /* minus the header */
}

static void read_status(hostfs_t *h, char *out)
{
    int32_t n = hf_read(h, HF_STATUS, (uint8_t *)out, 512u);

    out[(n < 0) ? 0 : n] = '\0';
}

/** Power the unit down the way the inactivity timer does; RAM is gone after. */
static void standby(void)
{
    host_deep_sleeps = 0u;
    host_deep_sleep_armed = true;
    if (setjmp(host_deep_sleep_jmp) == 0) {
        app_dispatch(APP_EVT_INACTIVITY);
        CHECK(0, "inactivity should have gone to Standby");
    }
    host_deep_sleep_armed = false;
    CHECK(host_deep_sleeps == 1u, "Standby reached");
}

/** Stage @p text as the host's SETTINGS.CSV, then pull the cable. */
static void host_edits_and_unplugs(hostfs_t *h, const char *text, uint32_t *feedback)
{
    hf_mount(h);
    CHECK(hf_create(h, HF_SETTINGS, text, (uint32_t)strlen(text)), "host copies SETTINGS.CSV");

    host_deep_sleeps = 0u;
    host_deep_sleep_armed = true;
    if (setjmp(host_deep_sleep_jmp) == 0) {
        app_dispatch(APP_EVT_USB_DETACH);
        *feedback = host_out_mask;
        CHECK(app_state() == ST_FEEDBACK, "unplugging after an edit shows how it went");
        pump_feedback();
        CHECK(0, "the unit never went to Standby");
    } else {
        CHECK(host_deep_sleeps == 1u, "Standby reached once");
        CHECK(app_state() == ST_SHUTDOWN, "in the shutdown state");
        CHECK(host_out_mask == 0u, "outputs off in Standby");
    }
    host_deep_sleep_armed = false;
}

void test_fsm(void)
{
    static hostfs_t h;
    static char csv[8192];
    static char status[513];
    uint32_t fb;
    const app_stats_t *st;

    printf("fsm end to end\n");

    host_flash_erase_all();
    app_init();
    CHECK(app_state() == ST_IDLE, "idle after init");
    st = app_get_stats();

    /* ---- taps: every card is recorded, none is "unknown" ---- */
    CHECK(tap(1000u, 10u) == OK_FB, "a card: green and a buzz");
    CHECK(tap(1007u, 12u) == OK_FB, "another card");
    CHECK(tap(1000u, 14u) == PLAT_OUT_VIBRATION, "same card again inside 10 s is a duplicate: buzz only");
    CHECK(st->scans_accepted == 2u && st->scans_duplicate == 1u, "stats %u/%u", st->scans_accepted, st->scans_duplicate);
    CHECK(tap(777777u, 60u) == OK_FB, "a card the device has never heard of is recorded all the same");
    CHECK(tap(1000u, 80u) == PLAT_OUT_VIBRATION, "a card already in this lecture is a duplicate, however late");
    CHECK(tap(777777u, 90u) == PLAT_OUT_VIBRATION, "...whoever it belongs to");

    /* Garbage is not a card. */
    app_dispatch(APP_EVT_TOUCH);
    host_cap_n = 0u;
    app_dispatch(APP_EVT_CAPTURE_FULL);
    CHECK(app_state() == ST_IDLE && st->false_wakes == 1u, "no decodable card is a false wake");

    /* ---- plug in: ATTEND.CSV has card and time, nothing else ---- */
    host_usb_starts = 0u;
    app_dispatch(APP_EVT_USB_ATTACH);
    CHECK(app_state() == ST_USB && host_usb_starts == 1u, "USB session started");
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

    /* Start a lecture and set the clock, in one edit. */
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 2\r\n");
    host_edits_and_unplugs(&h, csv, &fb);
    CHECK((fb & PLAT_OUT_LED_GREEN) != 0u && (fb & PLAT_OUT_LED_RED) == 0u, "applied: green");

    /* ---- reboot: the new lecture is in force ---- */
    app_init();
    CHECK(app_state() == ST_IDLE, "idle after reboot");
    CHECK(tap(777777u, 100u) == OK_FB, "the same card counts again in a new lecture");
    CHECK(tap(1000u, 120u) == OK_FB, "and so does every other");

    app_dispatch(APP_EVT_USB_ATTACH);
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 5u && usbs_file_size() == (1u + 5u) * CSV_ROW_BYTES, "records accumulate; the lecture marker is not a row (%u)", usbs_file_size());
    CHECK(strstr(csv, "2026-09-10,13:01:40,0000777777\r\n") != NULL, "the new tap");
    read_status(&h, status);
    CHECK(strstr(status, "Lecture      : EN2090 / Lecture 2") != NULL, "the device reports its lecture [%s]", status);

    /* ---- a refused edit: red, and nothing changes ---- */
    hf_mount(&h);
    CHECK(hf_create(&h, HF_SETTINGS, "x", 0u), "host leaves an empty file");
    host_deep_sleeps = 0u;
    host_deep_sleep_armed = true;
    if (setjmp(host_deep_sleep_jmp) == 0) {
        app_dispatch(APP_EVT_USB_DETACH);
        fb = host_out_mask;
        pump_feedback();
        CHECK(0, "should reach Standby");
    }
    host_deep_sleep_armed = false;
    CHECK((fb & PLAT_OUT_LED_RED) != 0u && (fb & PLAT_OUT_LED_GREEN) == 0u, "refused: red");

    app_init();
    app_dispatch(APP_EVT_USB_ATTACH);
    read_status(&h, status);
    hf_mount(&h);
    read_status(&h, status);
    CHECK(strstr(status, "Lecture      : EN2090 / Lecture 2") != NULL, "the lecture is as it was [%s]", status);

    /* ---- the device ID, and a flash write that silently stores the wrong bit ---- */
    host_corrupt_write_after(0u);
    snprintf(csv, sizeof(csv), "#DEVICE,4242\r\n");
    host_edits_and_unplugs(&h, csv, &fb);
    host_corrupt_write = 0u;
    CHECK((fb & PLAT_OUT_LED_RED) != 0u && (fb & PLAT_OUT_LED_GREEN) == 0u, "an ID that does not read back shows red");

    app_init();
    app_dispatch(APP_EVT_USB_ATTACH);
    snprintf(csv, sizeof(csv), "#DEVICE,4242\r\n");
    host_edits_and_unplugs(&h, csv, &fb);
    CHECK((fb & PLAT_OUT_LED_GREEN) != 0u, "the same edit without the fault is applied");
    app_init();
    app_dispatch(APP_EVT_USB_ATTACH);
    hf_mount(&h);
    read_status(&h, status);
    CHECK(strstr(status, "Device ID    : 0000004242") != NULL, "and the device reports it [%s]", status);
    CHECK(usbs_read(0u, (uint8_t *)csv, 1u) && fat12_rd32((const uint8_t *)&csv[39]) == 4242u, "as the volume serial");

    /* ---- plug in, look, unplug: no edit, no flash churn, straight to Standby ---- */
    {
        uint32_t e0 = host_flash_erases;

        host_deep_sleeps = 0u;
        host_deep_sleep_armed = true;
        if (setjmp(host_deep_sleep_jmp) == 0) {
            app_dispatch(APP_EVT_USB_DETACH);
            CHECK(0, "should have gone to Standby");
        }
        host_deep_sleep_armed = false;
        CHECK(host_deep_sleeps == 1u, "straight to Standby with nothing to report");
        CHECK(host_flash_erases == e0, "no pages erased by a look-only session");
    }

    /* ---- the clock, set from the USB drive ---- */
    app_init();
    app_dispatch(APP_EVT_USB_ATTACH);
    snprintf(csv, sizeof(csv), "#TIME,2031-12-31 23:59:00\r\n");
    host_edits_and_unplugs(&h, csv, &fb);
    app_init();
    {
        app_datetime_t now;

        plat_rtc_get(&now);
        CHECK(now.year == 2031u && now.month == 12u && now.day == 31u && now.hour == 23u && now.minute == 59u,
              "the typed time is in the RTC after the reboot");
    }
    CHECK(tap(555u, 5u) == OK_FB, "and the unit keeps recording");
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

    host_flash_erase_all();
    app_init();

    /* A card that stays on the reader, or is read twice in a row. */
    CHECK(tap(1000u, 10u) == OK_FB, "first tap recorded");
    CHECK(tap(1000u, 11u) == PLAT_OUT_VIBRATION, "second read of the same tap: duplicate");

    /* The student comes back later, after the unit slept (RAM, and so the
     * short-window table, is gone): the flash log remembers. */
    standby();
    app_init();
    CHECK(tap(1000u, hour) == PLAT_OUT_VIBRATION, "a tap after Standby is still a duplicate");
    CHECK(tap(1007u, hour + 100u) == OK_FB, "a different card is not");

    /* A different sitting, hours later with no lecture started. */
    CHECK(tap(1000u, 7u * hour) == OK_FB, "beyond APP_SESSION_MAX_AGE_S it counts again");

    /* A lecture is started from the USB drive. */
    app_dispatch(APP_EVT_USB_ATTACH);
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n");
    host_edits_and_unplugs(&h, csv, &fb);
    CHECK((fb & PLAT_OUT_LED_GREEN) != 0u, "lecture started: applied");
    app_init();
    CHECK(tap(1000u, 7u * hour + 100u) == OK_FB, "the new lecture starts everyone afresh");
    CHECK(tap(1000u, 7u * hour + 200u) == PLAT_OUT_VIBRATION, "and a second tap in it is a duplicate");
    CHECK(tap(1007u, 7u * hour + 210u) == OK_FB, "other cards sign in");

    /* An edit that leaves the names alone does not start a lecture. */
    app_dispatch(APP_EVT_USB_ATTACH);     /* the attach flushes the RAM buffer to flash */
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
    host_deep_sleeps = 0u;
    host_deep_sleep_armed = true;
    if (setjmp(host_deep_sleep_jmp) == 0) {
        app_dispatch(APP_EVT_USB_DETACH);
        pump_feedback();
        CHECK(0, "should have gone to Standby");
    }
    host_deep_sleep_armed = false;
    log_init(&ls);
    CHECK(log_total(&ls) == total, "an unchanged edit adds nothing to the log (%u vs %u)", log_total(&ls), total);
    app_init();
    CHECK(tap(1000u, 7u * hour + 300u) == PLAT_OUT_VIBRATION, "so the lecture carries on");

    /* The same names again, as a second lecture: #NEWSESSION. */
    app_dispatch(APP_EVT_USB_ATTACH);
    snprintf(csv, sizeof(csv), "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n#NEWSESSION,1\r\n");
    host_edits_and_unplugs(&h, csv, &fb);
    CHECK((fb & PLAT_OUT_LED_GREEN) != 0u, "second lecture with the same names: applied");
    app_init();
    CHECK(tap(1000u, 7u * hour + 400u) == OK_FB, "#NEWSESSION lets everyone sign in again");

    /* ATTEND.CSV: rows only, markers invisible. */
    app_dispatch(APP_EVT_USB_ATTACH);
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 6u, "6 attendance rows, no marker rows (%u)", rows_in(csv));
    CHECK(strstr(csv, "4294967") == NULL, "no marker id leaks into the file");
    CHECK(strstr(csv, "EN2090") == NULL, "no lecture name either: the PC keeps the lectures");
    read_status(&h, csv);
    CHECK(strstr(csv, "Lecture      : EN2090 / Lecture 1") != NULL, "the device still knows which lecture is running");
}
