/**
 * @file    battery.h
 * @brief   Level 2 (logic) — turn raw ADC counts into a battery verdict.
 *
 * Level 1 samples; this decides. Ratiometric against VREFINT so the result is
 * independent of VDDA, which matters because VDDA sags as the cell drains.
 */
#ifndef BATTERY_H
#define BATTERY_H

#include "app_types.h"

typedef enum {
    BATT_OK = 0,
    BATT_WARN,      /**< Usable, but warn the user on each scan. */
    BATT_CRITICAL   /**< Flush and shut down. */
} batt_state_t;

/** Convert a raw sample to battery millivolts. Returns 0 on a bad sample. */
uint32_t batt_millivolts(const app_adc_sample_t *s);

/** Classify millivolts against the thresholds in app_config.h. */
batt_state_t batt_classify(uint32_t millivolts);

#endif /* BATTERY_H */
