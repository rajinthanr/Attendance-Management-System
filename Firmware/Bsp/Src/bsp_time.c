/**
 * @file    bsp_time.c
 * @brief   Level 1 (HAL) — RTC calendar and the millisecond uptime.
 *
 * The RTC runs from the LSE in the backup domain, so the calendar keeps going
 * through Standby and resets. The uptime is SysTick's HAL_GetTick() while the
 * core runs. SysTick stops in Stop 2, so LPTIM1, counting the LSE without
 * pause, times each Stop 2 sleep, wakes the part at its end and lets
 * bsp_power.c add the time slept to the tick count.
 */
#include "bsp.h"
#include "app_debug.h"

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

/**
 * LPTIM1: a free-running 16-bit count of the LSE / 8 (4096 Hz, wraps every
 * 16 s).
 * Both the compare match and the wrap interrupt, so a Stop 2 sleep always
 * ends within one wrap of its start even if a compare write came too late,
 * and the ticks slept are never ambiguous. CubeMX clocks it from PCLK; this
 * puts it on the LSE.
 */
static bool s_lptim_ok;

/** Wait for an LPTIM1 write to land, but not for ever: it needs the LSE. */
static bool lptim_wait(uint32_t flag)
{
    uint32_t n;

    for (n = 0u; n < BSP_LPTIM_WAIT_LOOPS; n++) {
        if ((LPTIM1->ISR & flag) != 0u) {
            LPTIM1->ICR = flag;
            return true;
        }
    }
    return false;
}

static void lptim_fault(uint8_t why)
{
    s_lptim_ok = false;
    dbg_lptim_fault = why;
}

static void lptim_init(void)
{
    RCC_PeriphCLKInitTypeDef clk = { 0 };

    s_lptim_ok = false;

    clk.PeriphClockSelection = RCC_PERIPHCLK_LPTIM1;
    clk.Lptim1ClockSelection = RCC_LPTIM1CLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) {
        lptim_fault(1u);
        return;     /* no Stop 2, but the unit still works */
    }
    __HAL_RCC_LPTIM1_CLK_ENABLE();
    __HAL_RCC_LPTIM1_FORCE_RESET();
    __HAL_RCC_LPTIM1_RELEASE_RESET();

    /* IER and CFGR take writes only while the timer is disabled. Internal
     * clock (the LSE through the kernel clock mux), divided by 8. */
    _Static_assert(BSP_LPTIM_PRESC == 8u, "CFGR.PRESC below is /8");
    LPTIM1->CFGR = LPTIM_CFGR_PRESC_0 | LPTIM_CFGR_PRESC_1;
    LPTIM1->IER = LPTIM_IER_CMPMIE | LPTIM_IER_ARRMIE;
    LPTIM1->CR = LPTIM_CR_ENABLE;
    LPTIM1->ARR = 0xFFFFu;
    if (!lptim_wait(LPTIM_ISR_ARROK)) {
        LPTIM1->CR = 0u;
        lptim_fault(2u);    /* the LSE is not clocking it */
        return;
    }
    LPTIM1->CMP = 0u;
    if (!lptim_wait(LPTIM_ISR_CMPOK)) {
        LPTIM1->CR = 0u;
        lptim_fault(2u);
        return;
    }
    LPTIM1->CR = LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT;
    s_lptim_ok = true;
    dbg_lptim_fault = 0u;

    /* LPTIM1 is EXTI line 32, a direct line: unmasked, it wakes Stop 2. */
    SET_BIT(EXTI->IMR2, EXTI_IMR2_IM32);
    HAL_NVIC_SetPriority(LPTIM1_IRQn, BSP_PRIO_LPTIM, 0u);
    HAL_NVIC_EnableIRQ(LPTIM1_IRQn);
}

void bsp_time_init(void)
{
    rtc_init();
    lptim_init();
}

uint32_t bsp_lptim_count(void)
{
    uint32_t a, b;

    /* The counter runs on its own clock: trust two equal reads in a row. */
    do {
        a = LPTIM1->CNT;
        b = LPTIM1->CNT;
    } while (a != b);
    return a & 0xFFFFu;
}

bool bsp_lptim_ok(void)
{
    return s_lptim_ok;
}

bool bsp_lptim_wake_in(uint32_t ticks)
{
    if (!s_lptim_ok) {
        return false;
    }
    LPTIM1->ICR = LPTIM_ICR_CMPMCF | LPTIM_ICR_CMPOKCF;
    LPTIM1->CMP = (bsp_lptim_count() + ticks) & 0xFFFFu;
    /* The next CMP write must wait for this one; ~3 LSE cycles. Interrupts
     * are masked here, so a stopped LSE must not hang the part: give up and
     * stop using Stop 2. */
    if (!lptim_wait(LPTIM_ISR_CMPOK)) {
        lptim_fault(3u);
        return false;
    }
    return true;
}

void bsp_lptim_irq(void)
{
    /* The wake-up was the point; there is nothing else to do. */
    LPTIM1->ICR = LPTIM_ICR_CMPMCF | LPTIM_ICR_ARRMCF;
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
