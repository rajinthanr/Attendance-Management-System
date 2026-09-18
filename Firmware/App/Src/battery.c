/**
 * @file    battery.c
 * @brief   Level 2 (logic) — battery voltage reconstruction.
 */
#include "battery.h"
#include "app_config.h"

/* VREFINT_CAL in ST's system memory was measured at 3.0 V and 30 C. */
#define VREFINT_CAL_VDDA_MV  3000u
#define ADC_FULL_SCALE       4095u

uint32_t batt_millivolts(const app_adc_sample_t *s)
{
    if (s == NULL || s->vrefint_counts == 0u || s->vrefint_cal == 0u) {
        return 0u;
    }

    /* Recover the actual analog supply from the internal reference:
     *   VDDA = 3000 mV * VREFINT_CAL / VREFINT_measured
     * This is the standard ST relation and removes the supply from the
     * result entirely. */
    uint32_t vdda_mv = (VREFINT_CAL_VDDA_MV * (uint32_t)s->vrefint_cal)
                       / (uint32_t)s->vrefint_counts;

    /* Voltage at the divider node. */
    uint32_t node_mv = (vdda_mv * (uint32_t)s->vbat_counts) / ADC_FULL_SCALE;

    /* Undo the divider: Vbat = Vnode * (HIGH + LOW) / LOW.
     * Ordered so the multiply happens first; node_mv is at most ~3600 and the
     * ratio at most a few, so this stays far inside 32 bits. */
    uint32_t total = APP_BATT_DIV_HIGH_KOHM + APP_BATT_DIV_LOW_KOHM;

    return (node_mv * total) / APP_BATT_DIV_LOW_KOHM;
}

batt_state_t batt_classify(uint32_t millivolts)
{
    /* A zero reading means the sample was unusable, not that the battery is
     * flat. Treating it as critical would brick the unit on a bad ADC read. */
    if (millivolts == 0u) {
        return BATT_OK;
    }
    if (millivolts < APP_BATT_CUTOFF_MV) {
        return BATT_CRITICAL;
    }
    if (millivolts < APP_BATT_WARN_MV) {
        return BATT_WARN;
    }
    return BATT_OK;
}
