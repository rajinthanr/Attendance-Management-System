/**
 * @file    test_fsm_util.h
 * @brief   Driving the whole device on simulated time: cards held to the
 *          reader, the USB cable plugged and unplugged, the power button.
 *          Shared by test_fsm.c and test_cards.c.
 */
#ifndef TEST_FSM_UTIL_H
#define TEST_FSM_UTIL_H

#include "test_util.h"
#include "test_hostfs.h"

#include "app_config.h"
#include "app_debug.h"
#include "csv.h"
#include "device_cfg.h"
#include "log_store.h"
#include "usb_storage.h"
#include "platform_if.h"
#include "timeutil.h"

#define FB_GREEN_BIT   PLAT_OUT_LED_GREEN
#define FB_RED_BIT     PLAT_OUT_LED_RED
#define FB_VIB_BIT     PLAT_OUT_VIBRATION

/** Outputs driven when the last card was read, or when the last unplug reported. */
static uint32_t g_fb;

static inline app_datetime_t at(uint32_t second_offset)
{
    app_datetime_t dt = { 2026u, 9u, 10u, 13u, 0u, 0u };

    dt.hour = (uint8_t)(13u + (second_offset / 3600u));
    dt.minute = (uint8_t)((second_offset % 3600u) / 60u);
    dt.second = (uint8_t)(second_offset % 60u);
    return dt;
}

/** Fresh flash, a booted unit, the reader up. */
static inline void boot_fresh(void)
{
    host_flash_erase_all();
    host_ms = 0u;
    host_card_present = false;
    host_button = false;
    host_vbus = false;
    host_usb_configured = false;
    host_nfc_init_ok = true;
    host_deep_sleep_jmp = NULL;
    app_init();
    run_ms(600u);
}

/** Reboot with the flash as it is: what Standby and a wake-up amount to. */
static inline void reboot(void)
{
    host_card_present = false;
    app_init();
    run_ms(600u);
}

/**
 * Hold the card with number @p id to the reader at device time @p t seconds
 * (after 13:00:00) until it is read, then take it away. Returns what the unit
 * decided (APP_SCAN_*); @c g_fb holds the outputs it drove for feedback.
 */
static inline app_scan_result_t tap(uint32_t id, uint32_t t)
{
    const uint8_t uid[4] = { (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    app_datetime_t dt = at(t);
    uint32_t before = dbg_card_count;
    uint32_t i;

    host_set_time(&dt);
    host_card_set(uid, 4u, 0x08u);
    for (i = 0u; i < 3000u && dbg_card_count == before; i++) {
        app_task();
    }
    CHECK(dbg_card_count != before, "card %u was read", id);
    g_fb = host_out_mask;
    host_card_present = false;
    run_ms(1500u);
    CHECK(app_state() == ST_IDLE, "back to scanning after a tap");
    return (app_scan_result_t)dbg_scan_result;
}

/** Plug the cable into a host that enumerates the drive. */
static inline void plug(void)
{
    host_vbus = true;
    host_usb_configured = true;
    run_ms(300u);
    CHECK(app_state() == ST_USB && host_usb_started, "USB session started");
}

/**
 * Pull the cable. The unit applies what the host changed and plays a pattern
 * for it: returns the first outputs of that pattern (green or red with the
 * motor), or 0 when there was nothing to report.
 */
static inline uint32_t unplug(void)
{
    uint32_t i;

    host_vbus = false;
    for (i = 0u; i < 400u && app_state() == ST_USB; i++) {
        app_task();
    }
    CHECK(app_state() == ST_IDLE && !host_usb_started, "back to scanning after the cable came out");
    g_fb = ((host_out_mask & FB_VIB_BIT) != 0u) ? host_out_mask : 0u;
    run_ms(1500u);
    return g_fb;
}

/** Stage @p text as the host's SETTINGS.CSV (already plugged in), then pull the cable. */
static inline uint32_t host_edits_and_unplugs(hostfs_t *h, const char *text)
{
    hf_mount(h);
    CHECK(hf_create(h, HF_SETTINGS, text, (uint32_t)strlen(text)), "host copies SETTINGS.CSV");
    return unplug();
}

/** Read ATTEND.CSV through the volume into @p out (NUL terminated). */
static inline void read_attend(hostfs_t *h, char *out, uint32_t cap)
{
    int32_t n;

    hf_mount(h);
    n = hf_read(h, HF_ATTEND, (uint8_t *)out, cap - 1u);
    out[(n < 0) ? 0 : n] = '\0';
}

static inline void read_status(hostfs_t *h, char *out)
{
    int32_t n = hf_read(h, HF_STATUS, (uint8_t *)out, 512u);

    out[(n < 0) ? 0 : n] = '\0';
}

static inline uint32_t rows_in(const char *csv)
{
    uint32_t lines = 0u, i;

    for (i = 0u; csv[i] != '\0'; i++) {
        lines += (csv[i] == '\n');
    }
    return (lines > 0u) ? (lines - 1u) : 0u;      /* minus the header */
}

/** Hold the power button until the unit is off, then bring it back up. */
static inline void power_cycle(void)
{
    static jmp_buf jb;
    volatile int off = 0;

    host_deep_sleeps = 0u;
    host_deep_sleep_jmp = &jb;
    if (setjmp(jb) == 0) {
        host_button = true;
        run_ms(APP_BTN_LONG_MS + 1000u);
        CHECK(app_state() == ST_SHUTDOWN, "the long press starts the shutdown");
        host_button = false;
        run_ms(3000u);
    } else {
        off = 1;
    }
    host_deep_sleep_jmp = NULL;
    CHECK(off == 1 && host_deep_sleeps == 1u, "the unit reached Standby");
    host_button = false;
    reboot();
}

#endif /* TEST_FSM_UTIL_H */
