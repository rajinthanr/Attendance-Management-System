/**
 * @file    battery.c
 * @brief   Level 2 (logic) — battery voltage reconstruction.
 */
#include "battery.h"
#include "app_config.h"

/* VREFINT_CAL in ST's system memory was measured at 3.0 V and 30 C. */
#define VREFINT_CAL_VDDA_MV  3000u
#define ADC_FULL_SCALE       4095u

/* The ADC is specified for VDDA from 1.62 V to 3.6 V. A VREFINT reading that
 * implies anything outside that is a bad sample, not a supply. */
#define VDDA_MIN_MV          1620u
#define VDDA_MAX_MV          3600u

/* No single Li-ion cell, charging or not, reads above this. A higher result
 * means a saturated input (R8 open, say), not a full battery. */
#define VBAT_MAX_MV          4500u

void batt_evaluate(const app_adc_sample_t *s, batt_reading_t *out)
{
    out->status = BATT_SAMPLE_NO_VREFINT;
    out->vdda_mv = 0u;
    out->vbat_mv = 0u;

    if (s == NULL || s->vrefint_counts == 0u || s->vrefint_cal == 0u) {
        return;
    }

    /* Recover the actual analog supply from the internal reference:
     *   VDDA = 3000 mV * VREFINT_CAL / VREFINT_measured
     * This is the standard ST relation and removes the supply from the
     * result, so the reading stays right when the LDO drops out on a low
     * cell and VDDA sags below 3.3 V. */
    out->vdda_mv = (VREFINT_CAL_VDDA_MV * (uint32_t)s->vrefint_cal)
                   / (uint32_t)s->vrefint_counts;
    if (out->vdda_mv < VDDA_MIN_MV || out->vdda_mv > VDDA_MAX_MV) {
        out->status = BATT_SAMPLE_BAD_VDDA;
        return;
    }

    /* Voltage at the divider node. */
    uint32_t node_mv = (out->vdda_mv * (uint32_t)s->vbat_counts) / ADC_FULL_SCALE;

    /* Undo the divider: Vbat = Vnode * (HIGH + LOW) / LOW.
     * Ordered so the multiply happens first; node_mv is at most 3600 and the
     * resistor sum under 10 000, so this stays far inside 32 bits. */
    uint32_t total = APP_BATT_DIV_HIGH_KOHM + APP_BATT_DIV_LOW_KOHM;
    out->vbat_mv = (node_mv * total) / APP_BATT_DIV_LOW_KOHM;

    if (s->vbat_counts >= ADC_FULL_SCALE) {
        out->status = BATT_SAMPLE_SATURATED;
    } else if (out->vbat_mv > VBAT_MAX_MV) {
        out->status = BATT_SAMPLE_TOO_HIGH;
    } else {
        out->status = BATT_SAMPLE_OK;
    }
}

uint32_t batt_millivolts(const app_adc_sample_t *s)
{
    batt_reading_t r;

    batt_evaluate(s, &r);
    return (r.status == BATT_SAMPLE_OK) ? r.vbat_mv : 0u;
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

uint8_t batt_percent(uint32_t mv)
{
    /* Millivolts and percent at points along the curve; linear in between. */
    static const uint16_t k_mv[]  = { APP_BATT_CUTOFF_MV, 3600u, 3700u, 3750u, 3800u, 3850u,
                                      3900u, 3950u, 4000u, 4050u, 4100u, 4150u, 4200u };
    static const uint8_t  k_pct[] = { 0u, 5u, 15u, 25u, 40u, 55u, 63u, 70u, 78u, 85u, 91u, 96u, 100u };
    const uint32_t n = (uint32_t)(sizeof(k_mv) / sizeof(k_mv[0]));
    uint32_t i;

    if (mv <= k_mv[0]) {
        return 0u;
    }
    for (i = 1u; i < n; i++) {
        if (mv < k_mv[i]) {
            uint32_t span = (uint32_t)k_mv[i] - k_mv[i - 1u];
            uint32_t rise = (uint32_t)k_pct[i] - k_pct[i - 1u];

            return (uint8_t)(k_pct[i - 1u] + (((mv - k_mv[i - 1u]) * rise) / span));
        }
    }
    return 100u;
}
