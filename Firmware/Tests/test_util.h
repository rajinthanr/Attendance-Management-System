/**
 * @file    test_util.h
 * @brief   Shared by every test file: the CHECK macro, simulated-time helpers
 *          and the prototypes of the test groups.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "app_types.h"
#include "app_fsm.h"
#include "host_platform.h"

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

/** Run the state machine for @p ms of simulated time. */
static inline void run_ms(uint32_t ms)
{
    uint32_t end = host_ms + ms;

    while (host_ms < end) {
        app_task();
    }
}

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
void test_fsm_plugged_in(void);
void test_fsm_lectures(void);

/* ---- test_cards.c ---- */
void test_cards(void);

/* ---- test_session.c ---- */
void test_sessions(void);

#endif /* TEST_UTIL_H */
