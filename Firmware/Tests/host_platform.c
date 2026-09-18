/**
 * @file    host_platform.c
 * @brief   A Level 1 implementation for the host, backed by RAM.
 *
 * The existence of this file is the point of the layering: App/ compiles and
 * runs on a workstation with no STM32 headers anywhere, so the log format,
 * the decoder and the FAT image can be tested at desk speed.
 */
#include "platform_if.h"
#include "nv_layout.h"
#include <string.h>

#define HOST_FLASH_BYTES  (NV_PAGE_SIZE * 64u)

uint8_t  host_flash[HOST_FLASH_BYTES];
uint32_t host_out_mask;
uint32_t host_write_failures;   /* set to N to fail the Nth write, for tests */
static uint32_t s_writes;

static app_datetime_t s_now = { 2026u, 9u, 10u, 13u, 27u, 45u };

void host_flash_erase_all(void)
{
    memset(host_flash, 0xFF, sizeof(host_flash));
    s_writes = 0u;
    host_write_failures = 0u;
}

void host_set_time(const app_datetime_t *dt) { s_now = *dt; }

/* ---- outputs ---- */
void plat_out_write(uint32_t mask) { host_out_mask = mask; }

/* ---- power ---- */
void plat_sleep_idle(plat_idle_pred_t p) { (void)p; }
void plat_sleep_light(plat_idle_pred_t p) { (void)p; }
void plat_sleep_deep(void) { for (;;) { } }
void plat_critical_enter(void) { }
void plat_critical_exit(void) { }
app_boot_cause_t plat_boot_cause(void) { return APP_BOOT_POWER_ON; }

/* ---- rf ---- */
void plat_rf_power(bool on) { (void)on; }
void plat_rf_carrier(bool on) { (void)on; }
void plat_rf_capture_start(uint32_t *b, uint16_t c) { (void)b; (void)c; }
uint16_t plat_rf_capture_stop(void) { return 0u; }
uint32_t plat_rf_capture_hz(void) { return 1000000u; }

/* ---- touch ---- */
void plat_touch_power(bool on) { (void)on; }
void plat_touch_irq_enable(bool e) { (void)e; }
void plat_touch_recalibrate(void) { }

/* ---- timers ---- */
void plat_inactivity_restart(uint32_t ms) { (void)ms; }
void plat_inactivity_stop(void) { }
void plat_timer_start(uint32_t ms) { (void)ms; }
void plat_timer_stop(void) { }
uint32_t plat_uptime_ms(void) { return 0u; }

/* ---- rtc ---- */
void plat_rtc_get(app_datetime_t *o) { *o = s_now; }
void plat_rtc_set(const app_datetime_t *d) { s_now = *d; }
bool plat_rtc_is_valid(void) { return true; }

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

    memcpy(&host_flash[off], &value, 8u);
    return true;
}

bool plat_flash_erase(uint32_t off)
{
    uint32_t page = off / NV_PAGE_SIZE;
    if (((page + 1u) * NV_PAGE_SIZE) > HOST_FLASH_BYTES) { return false; }
    memset(&host_flash[page * NV_PAGE_SIZE], 0xFF, NV_PAGE_SIZE);
    return true;
}

/* ---- adc ---- */
bool plat_adc_sample(app_adc_sample_t *o)
{
    o->vbat_counts = 2400u;
    o->vrefint_counts = 1500u;
    o->vrefint_cal = 1655u;
    return true;
}

/* ---- usb ---- */
bool plat_usb_vbus_present(void) { return false; }
void plat_usb_start(void) { }
void plat_usb_stop(void) { }
