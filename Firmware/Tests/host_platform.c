/**
 * @file    host_platform.c
 * @brief   A Level 1 implementation for the host, backed by RAM.
 *
 * The existence of this file is the point of the layering: App/ compiles and
 * runs on a workstation with no STM32 headers anywhere, so the log format,
 * the card protocol and the FAT image can be tested at desk speed.
 *
 * Time only moves when the code under test sleeps: every plat_sleep_idle()
 * adds a millisecond, so N passes of app_task() are N ms of device time.
 */
#include "host_platform.h"
#include "platform_if.h"
#include "app_debug.h"
#include "nv_layout.h"
#include "crc.h"
#include "timeutil.h"
#include <string.h>

#define HOST_FLASH_BYTES  (NV_PAGE_SIZE * 64u)

uint8_t  host_flash[HOST_FLASH_BYTES];
uint32_t host_out_mask;
uint32_t host_write_failures;
uint32_t host_flash_writes;     /* double-words programmed since the last erase_all */
uint32_t host_flash_erases;     /* pages erased since the last erase_all */
uint32_t host_corrupt_write;    /* flip one bit of the double-word at this write count */
uint32_t host_rtc_sets;         /* plat_rtc_set() calls */
static uint32_t s_writes;

/* Live-debug globals; Core/Src/main.c defines them on the target. */
volatile uint32_t dbg_battery_mv;
volatile uint8_t  dbg_battery_state;
volatile uint16_t dbg_battery_counts;
volatile uint32_t dbg_battery_samples;
volatile uint8_t  dbg_battery_error;
volatile uint32_t dbg_battery_raw_mv;
volatile uint32_t dbg_vdda_mv;
volatile uint16_t dbg_vrefint_counts;
volatile uint16_t dbg_vrefint_cal;
volatile uint8_t  dbg_adc_error;
volatile uint32_t dbg_card_id;
volatile uint8_t  dbg_card_uid[10];
volatile uint8_t  dbg_card_uid_len;
volatile uint8_t  dbg_card_atqa[2];
volatile uint8_t  dbg_card_sak;
volatile uint32_t dbg_card_count;
volatile uint8_t  dbg_scan_result;
volatile bool     dbg_button_down;
volatile uint32_t dbg_button_short_count;
volatile uint32_t dbg_button_long_count;
volatile bool     dbg_nfc_ready;
volatile uint8_t  dbg_nfc_chip_id;
volatile bool     dbg_nfc_supply_3v3;
volatile uint8_t  dbg_nfc_amplitude;
volatile uint8_t  dbg_nfc_last_status;
volatile uint32_t dbg_nfc_polls;
volatile uint32_t dbg_nfc_errors;
volatile uint32_t dbg_nfc_collisions;
volatile uint32_t dbg_nfc_last_irq;
volatile uint32_t dbg_nfc_irq_pin_misses;
volatile uint8_t  dbg_state;
volatile uint8_t  dbg_boot_cause;
volatile uint32_t dbg_uptime_ms;
volatile bool     dbg_vbus;
volatile bool     dbg_usb_host;
volatile uint32_t dbg_records_ram;
volatile uint32_t dbg_records_flash;
volatile uint32_t dbg_records_free;
volatile uint32_t dbg_students;
volatile app_datetime_t dbg_now;
volatile app_datetime_t dbg_set_time;
volatile bool     dbg_set_time_request;

uint32_t host_ms;
bool     host_button;
bool     host_vbus;
bool     host_usb_configured;
bool     host_usb_started;
bool     host_usb_ejected;
uint16_t host_adc_vbat_counts = 1751u;   /* about 3.88 V */

jmp_buf *host_deep_sleep_jmp;
uint32_t host_deep_sleeps;

bool     host_nfc_init_ok = true;
bool     host_nfc_field;
bool     host_nfc_powered_down;
bool     host_nfc_collision;
bool     host_card_present;

static uint8_t s_uid[10];
static uint8_t s_uid_len;
static uint8_t s_sak;
static uint8_t s_level;        /* cascade level the card expects next */
static bool    s_card_ready;   /* answered REQA, not yet fully selected */

/* The RTC runs from a base date plus the elapsed host time. */
static app_epoch_t s_rtc_base = 842362065u;   /* 2026-09-10 13:27:45 */
static uint32_t    s_rtc_base_ms;
static bool        s_rtc_valid = true;

void host_flash_erase_all(void)
{
    memset(host_flash, 0xFF, sizeof(host_flash));
    s_writes = 0u;
    host_write_failures = 0u;
    host_flash_writes = 0u;
    host_flash_erases = 0u;
    host_corrupt_write = 0u;
}

/** Fail the (n+1)th flash write from now on. */
void host_fail_writes_after(uint32_t n) { host_write_failures = s_writes + n + 1u; }

/** Store the (n+1)th write from now with one bit wrong, reporting success. */
void host_corrupt_write_after(uint32_t n) { host_corrupt_write = s_writes + n + 1u; }

void host_set_time(const app_datetime_t *dt)
{
    s_rtc_base = time_to_epoch(dt);
    s_rtc_base_ms = host_ms;
}

void host_card_set(const uint8_t *uid, uint8_t len, uint8_t sak)
{
    memcpy(s_uid, uid, len);
    s_uid_len = len;
    s_sak = sak;
    host_card_present = true;
}

/* ---- outputs and inputs ---- */
void plat_out_write(uint32_t mask) { host_out_mask = mask; }
bool plat_button_pressed(void) { return host_button; }
bool plat_usb_vbus_present(void) { return host_vbus; }

/* ---- power ---- */
void plat_sleep_idle(plat_idle_pred_t p) { (void)p; host_ms++; }

void plat_sleep_deep(void)
{
    host_deep_sleeps++;
    if (host_deep_sleep_jmp != NULL) {
        longjmp(*host_deep_sleep_jmp, 1);
    }
    for (;;) { }
}

void plat_critical_enter(void) { }
void plat_critical_exit(void) { }
app_boot_cause_t plat_boot_cause(void) { return APP_BOOT_POWER_ON; }

/* ---- time ---- */
uint32_t plat_uptime_ms(void) { return host_ms; }

/* ---- rtc ---- */
void plat_rtc_get(app_datetime_t *o)
{
    time_from_epoch(s_rtc_base + ((host_ms - s_rtc_base_ms) / 1000u), o);
}

void plat_rtc_set(const app_datetime_t *d)
{
    host_set_time(d);
    s_rtc_valid = true;
    host_rtc_sets++;
}

bool plat_rtc_is_valid(void) { return s_rtc_valid; }

/* ---- nfc: a simulated ISO14443-A card ---- */
static uint8_t card_levels(void)
{
    return (s_uid_len == 4u) ? 1u : (s_uid_len == 7u) ? 2u : 3u;
}

/** The four UID-part bytes the card sends at cascade level @p level. */
static void card_uid_part(uint8_t level, uint8_t out[4])
{
    bool last = (uint8_t)(level + 1u) == card_levels();
    uint8_t start = (uint8_t)(level * 3u);

    if (last) {
        memcpy(out, &s_uid[start], 4u);
    } else {
        out[0] = 0x88u;
        memcpy(&out[1], &s_uid[start], 3u);
    }
}

bool plat_nfc_init(bool supply_3v3, uint8_t *chip_id)
{
    (void)supply_3v3;
    *chip_id = host_nfc_init_ok ? 0x2Au : 0x00u;
    host_nfc_powered_down = false;
    host_nfc_field = false;
    return host_nfc_init_ok;
}

bool plat_nfc_set_supply(bool supply_3v3) { (void)supply_3v3; return true; }

void plat_nfc_field(bool on)
{
    host_nfc_field = on;
    if (!on) {
        s_card_ready = false;   /* no field, no power: the card resets */
    }
}

plat_nfc_status_t plat_nfc_reqa(uint8_t atqa[2])
{
    if (!host_nfc_field || !host_card_present) {
        return PLAT_NFC_TIMEOUT;
    }
    if (host_nfc_collision) {
        return PLAT_NFC_COLLISION;
    }
    atqa[0] = (s_uid_len == 4u) ? 0x04u : 0x44u;
    atqa[1] = 0x00u;
    s_level = 0u;
    s_card_ready = true;
    return PLAT_NFC_OK;
}

plat_nfc_status_t plat_nfc_transceive(const uint8_t *tx, uint8_t tx_len,
                                      uint8_t flags, uint8_t *rx,
                                      uint8_t rx_cap, uint8_t *rx_len)
{
    static const uint8_t sel[3] = { 0x93u, 0x95u, 0x97u };
    uint8_t part[4];

    *rx_len = 0u;
    if (!host_nfc_field || !host_card_present || !s_card_ready ||
        s_level >= card_levels() || tx[0] != sel[s_level]) {
        return PLAT_NFC_TIMEOUT;
    }
    card_uid_part(s_level, part);

    if (tx_len == 2u && tx[1] == 0x20u && (flags & PLAT_NFC_ANTICOLLISION) != 0u) {
        if (rx_cap < 5u) { return PLAT_NFC_RX_ERROR; }
        memcpy(rx, part, 4u);
        rx[4] = (uint8_t)(part[0] ^ part[1] ^ part[2] ^ part[3]);
        *rx_len = 5u;
        return PLAT_NFC_OK;
    }

    if (tx_len == 7u && tx[1] == 0x70u && (flags & PLAT_NFC_TX_CRC) != 0u &&
        memcmp(&tx[2], part, 4u) == 0) {
        bool last = (uint8_t)(s_level + 1u) == card_levels();
        uint8_t sak = last ? s_sak : 0x04u;
        uint16_t crc = crc16_iso14443a(&sak, 1u);

        if (rx_cap < 3u) { return PLAT_NFC_RX_ERROR; }
        rx[0] = sak;
        rx[1] = (uint8_t)crc;
        rx[2] = (uint8_t)(crc >> 8);
        *rx_len = 3u;
        s_level++;
        return PLAT_NFC_OK;
    }
    return PLAT_NFC_TIMEOUT;
}

bool plat_nfc_measure_amplitude(uint8_t *raw) { *raw = 120u; return true; }
void plat_nfc_power_down(void) { host_nfc_powered_down = true; host_nfc_field = false; }

/* ---- flash ---- */
uint32_t plat_flash_page_size(void) { return NV_PAGE_SIZE; }

bool plat_flash_read(uint32_t off, void *dst, uint32_t len)
{
    if ((off + len) > HOST_FLASH_BYTES) { return false; }
    memcpy(dst, &host_flash[off], len);
    return true;
}

bool plat_flash_write_dw(uint32_t off, uint64_t value)
{
    if ((off + 8u) > HOST_FLASH_BYTES || (off % 8u) != 0u) { return false; }

    s_writes++;
    if (host_write_failures != 0u && s_writes >= host_write_failures) {
        return false;   /* simulate a power loss part-way through a flush */
    }

    /* Real flash can only clear bits, and the L4 refuses a second program of
     * the same double-word. Model both so the tests catch a format that
     * relies on rewriting. */
    uint64_t current;
    memcpy(&current, &host_flash[off], 8u);
    if (current != NV_ERASED_DW) { return false; }

    if (host_corrupt_write != 0u && s_writes == host_corrupt_write) {
        value ^= 1u;            /* a write that "succeeds" but stores the wrong bit */
    }
    memcpy(&host_flash[off], &value, 8u);
    host_flash_writes++;
    return true;
}

bool plat_flash_erase(uint32_t off)
{
    uint32_t page = off / NV_PAGE_SIZE;
    if (((page + 1u) * NV_PAGE_SIZE) > HOST_FLASH_BYTES) { return false; }
    memset(&host_flash[page * NV_PAGE_SIZE], 0xFF, NV_PAGE_SIZE);
    host_flash_erases++;
    return true;
}

/* ---- adc ---- */
bool plat_adc_sample(app_adc_sample_t *o)
{
    o->vbat_counts = host_adc_vbat_counts;
    o->vrefint_counts = 1500u;   /* with the CAL value below: VDDA 3310 mV */
    o->vrefint_cal = 1655u;
    return true;
}

/* ---- usb ---- */
void plat_usb_start(void) { host_usb_started = true; host_usb_ejected = false; }
void plat_usb_stop(void) { host_usb_started = false; }
bool plat_usb_configured(void) { return host_usb_started && host_usb_configured; }
bool plat_usb_ejected(void) { return host_usb_started && host_usb_ejected; }
