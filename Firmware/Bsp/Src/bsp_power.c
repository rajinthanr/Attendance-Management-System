/**
 * @file    bsp_power.c
 * @brief   Level 1 (HAL) — sleep modes, brown-out detection, boot cause.
 *
 * The two depths the application asks for map onto:
 *   plat_sleep_idle()   Sleep    core clock gated until the next interrupt
 *   plat_sleep_deep()   Standby  SRAM lost, only the WKUP pin gets out
 *
 * Stop 2 comes back with the move to interrupts: in polling mode the loop
 * needs SysTick, which Stop 2 halts.
 */
#include "bsp.h"
#include "app_events.h"

static app_boot_cause_t s_boot_cause;

void bsp_power_init(void)
{
    /* Latch why we booted before the flags are cleared; the application uses
     * this to tell a Standby wake from a cold start. */
    if (__HAL_PWR_GET_FLAG(PWR_FLAG_SB) != RESET) {
        s_boot_cause = APP_BOOT_FROM_STANDBY;
    } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET ||
               __HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != RESET) {
        s_boot_cause = APP_BOOT_WATCHDOG;
    } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST) != RESET ||
               __HAL_RCC_GET_FLAG(RCC_FLAG_BORRST) != RESET) {
        s_boot_cause = APP_BOOT_POWER_ON;
    } else {
        s_boot_cause = APP_BOOT_OTHER;
    }

    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_SB | PWR_FLAG_WU);
    __HAL_RCC_CLEAR_RESET_FLAGS();

#if BSP_ENABLE_BATTERY_PVD
    /* Programmable voltage detector on the external input, which the board
     * ties to the battery divider. Levels 0..6 watch VDD, which a regulator
     * holds steady until it drops out entirely and by then it is too late to
     * write flash; level 7 compares PVD_IN against VREFINT and so actually
     * tracks the cell. */
    PWR_PVDTypeDef pvd = { 0 };
    pvd.PVDLevel = PWR_PVDLEVEL_7;
    pvd.Mode = PWR_PVD_MODE_IT_RISING;
    HAL_PWR_ConfigPVD(&pvd);

    HAL_NVIC_SetPriority(PVD_PVM_IRQn, BSP_PRIO_PVD, 0u);
    HAL_NVIC_EnableIRQ(PVD_PVM_IRQn);
    HAL_PWR_EnablePVD();
#else
    /* The generated HAL_MspInit() switches the PVD on; nothing listens to it
     * while the battery is polled through the ADC. */
    HAL_PWR_DisablePVD();
#endif
}

app_boot_cause_t plat_boot_cause(void)
{
    return s_boot_cause;
}

/* ------------------------------------------------------------------------ */
/* Critical sections                                                        */
/* ------------------------------------------------------------------------ */

static volatile uint32_t s_critical_nesting;
static uint32_t s_saved_primask;

void plat_critical_enter(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    if (s_critical_nesting == 0u) {
        s_saved_primask = primask;
    }
    s_critical_nesting++;
}

void plat_critical_exit(void)
{
    if (s_critical_nesting > 0u) {
        s_critical_nesting--;
        if (s_critical_nesting == 0u && (s_saved_primask & 1u) == 0u) {
            __enable_irq();
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Sleep modes                                                              */
/* ------------------------------------------------------------------------ */

/**
 * Mask interrupts and re-test @p still_idle.
 *
 * WFI wakes on an interrupt that is merely pending, even with PRIMASK set, so
 * masking here does not cost a wake-up. It only guarantees that no interrupt
 * can slip in between the test and the sleep instruction. The handler runs as
 * soon as the mask is lifted.
 *
 * @return true if the caller should go ahead and sleep. Interrupts are left
 *         masked in that case, for the caller to restore after the WFI.
 */
static bool sleep_arm(plat_idle_pred_t still_idle, uint32_t *saved_primask)
{
    *saved_primask = __get_PRIMASK();
    __disable_irq();

    if (still_idle != NULL && !still_idle()) {
        if ((*saved_primask & 1u) == 0u) {
            __enable_irq();
        }
        return false;
    }
    return true;
}

static void sleep_release(uint32_t saved_primask)
{
    if ((saved_primask & 1u) == 0u) {
        __enable_irq();
    }
}

void plat_sleep_idle(plat_idle_pred_t still_idle)
{
    uint32_t primask;

    /* With a debugger attached the core stays awake, as the bring-up loop
     * did. Reads the debugger makes while the core sits in WFI can come back
     * as zero, which makes Live Expressions flicker. Nothing depends on the
     * sleep for timing: the loop runs off plat_uptime_ms(). */
    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0u) {
        return;
    }

    if (!sleep_arm(still_idle, &primask)) {
        return;
    }

    HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
    sleep_release(primask);
}

void plat_sleep_deep(void)
{
    /* Everything the flow chart calls for before Standby: actuators down,
     * wake sources reduced to the power button alone. The application has
     * already put the reader into its power-down mode. */
    plat_out_write(0u);
    HAL_PWR_DisablePVD();

    /* Standby wakes on WKUP1 only, and wakes through reset, so the pending
     * flag has to be clear or the part would come straight back out. */
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN1);
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_LOW);

    /* GPIO pull-ups are released in Standby unless they are explicitly
     * retained. Without this the button pin floats and the part wakes on
     * whatever noise reaches it, which on a battery device shows up as a
     * unit that will not stay off. */
    (void)HAL_PWREx_EnableGPIOPullUp(PWR_GPIO_A, PWR_GPIO_BIT_0);
    HAL_PWREx_EnablePullUpPullDownConfig();

    HAL_SuspendTick();
    HAL_PWR_EnterSTANDBYMode();

    /* Standby exits through reset, so this is unreachable. Looping rather
     * than returning keeps the noreturn contract honest if a future part
     * were to behave differently. */
    for (;;) {
        __WFI();
    }
}

/* ------------------------------------------------------------------------ */
/* PVD                                                                      */
/* ------------------------------------------------------------------------ */

void HAL_PWR_PVDCallback(void)
{
    app_event_post(APP_EVT_LOW_BATTERY);
}
