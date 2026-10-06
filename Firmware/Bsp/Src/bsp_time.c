/**
 * @file    bsp_time.c
 * @brief   Level 1 (HAL) — RTC calendar and the millisecond uptime.
 *
 * The RTC runs from the LSE in the backup domain, so the calendar keeps going
 * through Standby and resets. The uptime is SysTick's HAL_GetTick(), which is
 * enough while the main loop polls and never enters Stop 2.
 */
#include "bsp.h"

RTC_HandleTypeDef   hbsp_rtc;

/* ------------------------------------------------------------------------ */
/* Init                                                                     */
/* ------------------------------------------------------------------------ */

static void rtc_init(void)
{
    __HAL_RCC_RTC_ENABLE();

    hbsp_rtc.Instance = RTC;
    hbsp_rtc.Init.HourFormat = RTC_HOURFORMAT_24;
    /* 32768 = 128 * 256, the combination ST specifies for a 1 Hz calendar
     * from the LSE with the lowest possible power. */
    hbsp_rtc.Init.AsynchPrediv = 127u;
    hbsp_rtc.Init.SynchPrediv = 255u;
    hbsp_rtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    hbsp_rtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    hbsp_rtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hbsp_rtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;

    if (HAL_RTC_Init(&hbsp_rtc) != HAL_OK) {
        Error_Handler();
    }

    /* The calendar is left alone: it survives Standby and reset, and the
     * application seeds it when plat_rtc_is_valid() says it was never set. */
}

void bsp_time_init(void)
{
    rtc_init();
}

/* ------------------------------------------------------------------------ */
/* platform_if: RTC                                                         */
/* ------------------------------------------------------------------------ */

void plat_rtc_get(app_datetime_t *out)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;

    /* Reading time locks the shadow registers; the date read releases them.
     * Doing it in the other order can return a date one day stale. */
    (void)HAL_RTC_GetTime(&hbsp_rtc, &t, RTC_FORMAT_BIN);
    (void)HAL_RTC_GetDate(&hbsp_rtc, &d, RTC_FORMAT_BIN);

    out->year = (uint16_t)(2000u + d.Year);
    out->month = d.Month;
    out->day = d.Date;
    out->hour = t.Hours;
    out->minute = t.Minutes;
    out->second = t.Seconds;
}

void plat_rtc_set(const app_datetime_t *dt)
{
    RTC_TimeTypeDef t = { 0 };
    RTC_DateTypeDef d = { 0 };

    d.Year = (uint8_t)(dt->year - 2000u);
    d.Month = dt->month;
    d.Date = dt->day;
    d.WeekDay = RTC_WEEKDAY_MONDAY;   /* not used; the calendar recomputes it */

    t.Hours = dt->hour;
    t.Minutes = dt->minute;
    t.Seconds = dt->second;
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    if (HAL_RTC_SetTime(&hbsp_rtc, &t, RTC_FORMAT_BIN) == HAL_OK &&
        HAL_RTC_SetDate(&hbsp_rtc, &d, RTC_FORMAT_BIN) == HAL_OK) {
        HAL_RTCEx_BKUPWrite(&hbsp_rtc, BSP_BKP_RTC_VALID, BSP_BKP_RTC_MAGIC);
    }
}

bool plat_rtc_is_valid(void)
{
    return (HAL_RTCEx_BKUPRead(&hbsp_rtc, BSP_BKP_RTC_VALID) == BSP_BKP_RTC_MAGIC);
}

/* ------------------------------------------------------------------------ */
/* platform_if: uptime                                                      */
/* ------------------------------------------------------------------------ */

uint32_t plat_uptime_ms(void)
{
    return HAL_GetTick();
}
