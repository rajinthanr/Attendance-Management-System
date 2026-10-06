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

/** Whether a sample can be trusted, and if not, why. */
typedef enum {
    BATT_SAMPLE_OK = 0,
    BATT_SAMPLE_NO_VREFINT,   /**< VREFINT or its calibration word read 0. */
    BATT_SAMPLE_BAD_VDDA,     /**< VREFINT implies VDDA outside 1.62-3.6 V. */
    BATT_SAMPLE_SATURATED,    /**< Battery input at full scale. */
    BATT_SAMPLE_TOO_HIGH      /**< Above any single Li-ion cell (4.5 V). */
} batt_sample_t;

typedef struct {
    batt_sample_t status;
    uint32_t vdda_mv;   /**< Analog supply recovered from VREFINT. */
    uint32_t vbat_mv;   /**< Battery voltage, computed even when rejected. */
} batt_reading_t;

/**
 * Work out VDDA and the battery voltage from a raw sample and judge it. The
 * voltages are filled in whenever they can be computed, so a rejected sample
 * can still be inspected.
 */
void batt_evaluate(const app_adc_sample_t *s, batt_reading_t *out);

/** Battery millivolts, or 0 when batt_evaluate() rejects the sample. */
uint32_t batt_millivolts(const app_adc_sample_t *s);

/** Classify millivolts against the thresholds in app_config.h. */
batt_state_t batt_classify(uint32_t millivolts);

#endif /* BATTERY_H */
