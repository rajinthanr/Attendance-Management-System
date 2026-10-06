/**
 * @file    host_platform.h
 * @brief   Test hooks into the RAM-backed host platform.
 */
#ifndef HOST_PLATFORM_H
#define HOST_PLATFORM_H

#include <setjmp.h>
#include "app_types.h"

extern uint8_t  host_flash[];
extern uint32_t host_write_failures;  /* set to N to fail the Nth write */
extern uint32_t host_out_mask;        /* last plat_out_write() */

extern uint32_t host_ms;              /* plat_uptime_ms(); plat_sleep_idle() adds 1 */
extern bool     host_button;
extern bool     host_vbus;
extern bool     host_usb_configured;
extern bool     host_usb_started;
extern uint16_t host_adc_vbat_counts;

extern jmp_buf *host_deep_sleep_jmp;  /* plat_sleep_deep() longjmps here */
extern uint32_t host_deep_sleeps;

/* Simulated reader and card. */
extern bool     host_nfc_init_ok;
extern bool     host_nfc_field;
extern bool     host_nfc_powered_down;
extern bool     host_nfc_collision;   /* REQA answers with a collision */
extern bool     host_card_present;

extern uint32_t host_flash_writes;
extern uint32_t host_flash_erases;
extern uint32_t host_corrupt_write;
extern uint32_t host_rtc_sets;

void host_flash_erase_all(void);
void host_fail_writes_after(uint32_t n);
void host_corrupt_write_after(uint32_t n);
void host_set_time(const app_datetime_t *dt);
void host_card_set(const uint8_t *uid, uint8_t len, uint8_t sak);

#endif /* HOST_PLATFORM_H */
