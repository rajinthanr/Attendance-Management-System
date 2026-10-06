/**
 * @file    test_util.h
 * @brief   Shared by every test file: the CHECK macro and the host stub's knobs.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "app_types.h"

extern int g_fail;
extern int g_run;

#define CHECK(cond, ...) do {                        \
    g_run++;                                         \
    if (!(cond)) {                                   \
        g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);\
        printf(__VA_ARGS__);                         \
        printf("\n");                                \
    }                                                \
} while (0)

/* ---- host_platform.c ---- */
extern uint8_t  host_flash[];
extern uint32_t host_write_failures;
extern uint32_t host_flash_writes;
extern uint32_t host_flash_erases;
extern uint32_t host_corrupt_write;
extern uint32_t host_out_mask;
extern uint32_t *host_cap_buf;
extern uint16_t host_cap_n;
extern jmp_buf  host_deep_sleep_jmp;
extern bool     host_deep_sleep_armed;
extern uint32_t host_deep_sleeps;
extern uint32_t host_timer_ms;
extern uint32_t host_usb_starts;
extern uint32_t host_usb_stops;
extern uint32_t host_rtc_sets;
void host_flash_erase_all(void);
void host_set_time(const app_datetime_t *dt);
void host_fail_writes_after(uint32_t n);
void host_corrupt_write_after(uint32_t n);

/* ---- test_main.c ---- */

/** Encode one EM4100 card and fill the host capture buffer with its edges. */
void present_card(uint32_t id);

/* ---- test_settings.c ---- */
void test_csv(void);
void test_device_cfg(void);
void test_settings_parser(void);
void test_usb_volume(void);
void test_usb_settings(void);
void test_usb_robustness(void);

/* ---- test_fsm.c ---- */
void test_fsm(void);
void test_fsm_sessions(void);

/* ---- test_cards.c ---- */
void test_cards(void);

/* ---- test_session.c ---- */
void test_sessions(void);

#endif /* TEST_UTIL_H */
