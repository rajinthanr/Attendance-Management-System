/**
 * @file    bsp_adc.c
 * @brief   Level 1 (HAL) — battery sampling.
 *
 * Returns raw counts and the factory calibration word. Turning those into
 * millivolts is battery.c's job, in Level 2, where it can be unit tested.
 *
 * The ADC is powered down between samples: its analog front end costs a couple
 * of hundred microamps, which would dwarf everything else in a device that is
 * asleep almost all the time.
 */
#include "bsp.h"

ADC_HandleTypeDef hbsp_adc;

void bsp_adc_init(void)
{
    /* Nothing is configured here on purpose. The ADC is brought up inside
     * plat_adc_sample() and shut down again on the way out. */
    hbsp_adc.Instance = BSP_BATT_ADC;
}

static bool adc_start(void)
{
    __HAL_RCC_ADC_CLK_ENABLE();

    hbsp_adc.Instance = BSP_BATT_ADC;
    hbsp_adc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hbsp_adc.Init.Resolution = ADC_RESOLUTION_12B;
    hbsp_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hbsp_adc.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hbsp_adc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hbsp_adc.Init.LowPowerAutoWait = DISABLE;
    hbsp_adc.Init.ContinuousConvMode = DISABLE;
    hbsp_adc.Init.NbrOfConversion = 1u;
    hbsp_adc.Init.DiscontinuousConvMode = DISABLE;
    hbsp_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hbsp_adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hbsp_adc.Init.DMAContinuousRequests = DISABLE;
    hbsp_adc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    hbsp_adc.Init.OversamplingMode = DISABLE;

    if (HAL_ADC_Init(&hbsp_adc) != HAL_OK) {
        return false;
    }

    /* Single-ended calibration is required after every power-up of the
     * analog block, and this one is powered up per sample. */
    return (HAL_ADCEx_Calibration_Start(&hbsp_adc, ADC_SINGLE_ENDED) == HAL_OK);
}

static bool adc_read_channel(uint32_t channel, uint16_t *out)
{
    ADC_ChannelConfTypeDef c = { 0 };

    c.Channel = channel;
    c.Rank = ADC_REGULAR_RANK_1;
    /* 640.5 cycles: the battery divider is 2.35 MOhm of source impedance and
     * VREFINT needs a long window of its own. */
    c.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
    c.SingleDiff = ADC_SINGLE_ENDED;
    c.OffsetNumber = ADC_OFFSET_NONE;
    c.Offset = 0u;

    if (HAL_ADC_ConfigChannel(&hbsp_adc, &c) != HAL_OK) {
        return false;
    }
    if (HAL_ADC_Start(&hbsp_adc) != HAL_OK) {
        return false;
    }
    if (HAL_ADC_PollForConversion(&hbsp_adc, 10u) != HAL_OK) {
        (void)HAL_ADC_Stop(&hbsp_adc);
        return false;
    }

    *out = (uint16_t)HAL_ADC_GetValue(&hbsp_adc);
    (void)HAL_ADC_Stop(&hbsp_adc);
    return true;
}

bool plat_adc_sample(app_adc_sample_t *out)
{
    bool ok;

    if (out == NULL) {
        return false;
    }

    if (!adc_start()) {
        __HAL_RCC_ADC_CLK_DISABLE();
        return false;
    }

    ok = adc_read_channel(BSP_BATT_ADC_CHANNEL, &out->vbat_counts) &&
         adc_read_channel(ADC_CHANNEL_VREFINT, &out->vrefint_counts);

    /* Measured at 3.0 V during production test and burned into system memory;
     * battery.c uses it to back out the actual supply. */
    out->vrefint_cal = *VREFINT_CAL_ADDR;

    (void)HAL_ADC_DeInit(&hbsp_adc);
    __HAL_RCC_ADC_CLK_DISABLE();

    return ok;
}
