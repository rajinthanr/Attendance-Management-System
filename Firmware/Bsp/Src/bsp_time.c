/**
 * @file    bsp_time.c
 * @brief   Level 1 (HAL) — RTC calendar and the two low-power timers.
 *
 * Both timers are clocked from the LSE rather than from a TIMx on PCLK, so
 * they keep counting in Stop 2. That is what makes the flow chart's "light
 * sleep" a real sleep rather than a slower idle loop.
 */
#include "bsp.h"
#include "app_events.h"

RTC_HandleTypeDef   hbsp_rtc;
LPTIM_HandleTypeDef hbsp_lptim_inact;
LPTIM_HandleTypeDef hbsp_lptim_delay;

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

    /* Only seed the calendar on a truly cold start. The RTC and its backup
     * registers survive Standby and reset, so re-seeding here would throw the
     * clock away every time the user pressed the power button. */
    if (HAL_RTCEx_BKUPRead(&hbsp_rtc, BSP_BKP_RTC_VALID) != BSP_BKP_RTC_MAGIC) {
        RTC_DateTypeDef d = { 0 };
        RTC_TimeTypeDef t = { 0 };

        d.Year = 26u;   /* 2026-01-01, a placeholder until a host sets it */
        d.Month = RTC_MONTH_JANUARY;
        d.Date = 1u;
        d.WeekDay = RTC_WEEKDAY_THURSDAY;
        (void)HAL_RTC_SetDate(&hbsp_rtc, &d, RTC_FORMAT_BIN);

        t.Hours = 0u;
        t.Minutes = 0u;
        t.Seconds = 0u;
        t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        t.StoreOperation = RTC_STOREOPERATION_RESET;
        (void)HAL_RTC_SetTime(&hbsp_rtc, &t, RTC_FORMAT_BIN);
    }
}

static void lptim_init(void)
{
    __HAL_RCC_LPTIM1_CLK_ENABLE();
    __HAL_RCC_LPTIM2_CLK_ENABLE();

    hbsp_lptim_inact.Instance = BSP_INACT_LPTIM;
    hbsp_lptim_inact.Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
    hbsp_lptim_inact.Init.Clock.Prescaler = BSP_INACT_PRESCALER;
    hbsp_lptim_inact.Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
    hbsp_lptim_inact.Init.OutputPolarity = LPTIM_OUTPUTPOLARITY_HIGH;
    hbsp_lptim_inact.Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
    hbsp_lptim_inact.Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
    if (HAL_LPTIM_Init(&hbsp_lptim_inact) != HAL_OK) {
        Error_Handler();
    }

    /* Filled in field by field rather than copied from the handle above:
     * a struct copy would also carry across that handle's State, and
     * HAL_LPTIM_Init skips its MspInit for anything not in RESET state. */
    hbsp_lptim_delay.Instance = BSP_DELAY_LPTIM;
    hbsp_lptim_delay.Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
    hbsp_lptim_delay.Init.Clock.Prescaler = BSP_DELAY_PRESCALER;
    hbsp_lptim_delay.Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
    hbsp_lptim_delay.Init.OutputPolarity = LPTIM_OUTPUTPOLARITY_HIGH;
    hbsp_lptim_delay.Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
    hbsp_lptim_delay.Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
    if (HAL_LPTIM_Init(&hbsp_lptim_delay) != HAL_OK) {
        Error_Handler();
    }

    HAL_NVIC_SetPriority(LPTIM1_IRQn, BSP_PRIO_LPTIM, 0u);
    HAL_NVIC_EnableIRQ(LPTIM1_IRQn);
    HAL_NVIC_SetPriority(LPTIM2_IRQn, BSP_PRIO_LPTIM, 0u);
    HAL_NVIC_EnableIRQ(LPTIM2_IRQn);
}

void bsp_time_init(void)
{
    rtc_init();
    lptim_init();
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
/* platform_if: timers                                                      */
/* ------------------------------------------------------------------------ */

/** Convert milliseconds to LPTIM counts, saturating at the 16-bit ARR. */
static uint32_t ms_to_counts(uint32_t ms, uint32_t tick_hz)
{
    uint32_t counts = (uint32_t)(((uint64_t)ms * tick_hz) / 1000u);

    if (counts == 0u) {
        counts = 1u;        /* ARR of 0 is not a legal LPTIM period */
    }
    if (counts > 0xFFFFu) {
        counts = 0xFFFFu;
    }
    return counts;
}

void plat_inactivity_restart(uint32_t ms)
{
    (void)HAL_LPTIM_Counter_Stop_IT(&hbsp_lptim_inact);
    (void)HAL_LPTIM_Counter_Start_IT(&hbsp_lptim_inact,
                                     ms_to_counts(ms, BSP_INACT_TICK_HZ));
}

void plat_inactivity_stop(void)
{
    (void)HAL_LPTIM_Counter_Stop_IT(&hbsp_lptim_inact);
}

void plat_timer_start(uint32_t ms)
{
    (void)HAL_LPTIM_Counter_Stop_IT(&hbsp_lptim_delay);
    (void)HAL_LPTIM_Counter_Start_IT(&hbsp_lptim_delay,
                                     ms_to_counts(ms, BSP_DELAY_TICK_HZ));
}

void plat_timer_stop(void)
{
    (void)HAL_LPTIM_Counter_Stop_IT(&hbsp_lptim_delay);
}

uint32_t plat_uptime_ms(void)
{
    /* Derived from the RTC rather than SysTick, which stops in Stop 2 and
     * would make this jump backwards relative to wall time. One-second
     * resolution is all the callers need. */
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;

    (void)HAL_RTC_GetTime(&hbsp_rtc, &t, RTC_FORMAT_BIN);
    (void)HAL_RTC_GetDate(&hbsp_rtc, &d, RTC_FORMAT_BIN);

    return ((((uint32_t)t.Hours * 3600u) +
             ((uint32_t)t.Minutes * 60u) +
             (uint32_t)t.Seconds) * 1000u);
}

/* ------------------------------------------------------------------------ */
/* LPTIM interrupt: period elapsed                                          */
/* ------------------------------------------------------------------------ */

void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *h)
{
    /* Both timers are one-shot from the application's point of view, so stop
     * them here rather than let the LPTIM free-run and fire again. */
    if (h->Instance == BSP_INACT_LPTIM) {
        (void)HAL_LPTIM_Counter_Stop_IT(h);
        app_event_post(APP_EVT_INACTIVITY);
    } else if (h->Instance == BSP_DELAY_LPTIM) {
        (void)HAL_LPTIM_Counter_Stop_IT(h);
        app_event_post(APP_EVT_TIMER);
    }
}
