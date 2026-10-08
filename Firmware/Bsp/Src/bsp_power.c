/**
 * @file    bsp_power.c
 * @brief   Level 1 (HAL) — sleep modes, brown-out detection, boot cause.
 *
 * The two depths the application asks for map onto:
 *   plat_sleep_until()  Sleep    core clock gated until the next interrupt
 *                       Stop 2   when allowed and long enough; LPTIM1 wakes it
 *   plat_sleep_deep()   Standby  SRAM lost, only the WKUP pin gets out
 *
 * SysTick halts in Stop 2, so stop2_for() measures the sleep on LPTIM1 (LSE)
 * and adds it to HAL's tick: plat_uptime_ms() never notices the gap.
 */
#include "bsp.h"
#include "app_debug.h"
#include "app_events.h"

static app_boot_cause_t s_boot_cause;

/* Switch the PVD off and deselect its external input. With PLS still at 7
 * (PVD_IN) and the PVD off, PB7 pulls the battery divider node up to VDD,
 * so PA7 reads full scale (measured on the board: 4079 counts instead of
 * ~1250). Level 0 watches VDD internally and leaves PB7 alone. */
/* Clear DBG_SLEEP, DBG_STOP and DBG_STANDBY. With them set, Stop 2 and
 * Standby keep the digital core powered and a clock running. */
static void debug_low_power_off(void)
{
    DBGMCU->CR &= ~(DBGMCU_CR_DBG_SLEEP | DBGMCU_CR_DBG_STOP |
                    DBGMCU_CR_DBG_STANDBY);
}

static void pvd_off(void)
{
    HAL_PWR_DisablePVD();
    MODIFY_REG(PWR->CR2, PWR_CR2_PLS, PWR_PVDLEVEL_0);
}

/**
 * Every pass of ST's SWD flash loader (STM32CubeProgrammer, and CubeIDE
 * through the ST-LINK GDB server) toggles FLASH_SR.PEMPTY: writing 1 to it
 * flips it, and the loader clears status flags with a mask that includes it.
 * Measured on the board: 0 before programming, 1 after, 0 after a second
 * pass. While it is set, every reset that is not a power-on boots the system
 * bootloader (DFU), since the chip re-reads it only at power-on or option-byte
 * reload; address 0 then maps the bootloader, so a program a debugger starts
 * after such a reset takes its interrupts from the bootloader's vectors; and
 * the HAL takes the flag for a flash error, failing the next log write.
 *
 * So, first thing in main(): point VTOR at our own vector table, whatever is
 * mapped at 0, and flip the flag back (we are running from main flash, so it
 * is not empty).
 */
void bsp_early_init(void)
{
    SCB->VTOR = FLASH_BASE;
    __DSB();
    __ISB();
    if ((FLASH->SR & FLASH_SR_PEMPTY) != 0u) {
        FLASH->SR = FLASH_SR_PEMPTY;
        dbg_pempty_cleared = 1u;
    }
}

void bsp_power_init(void)
{
    /* Debug clocks off in the low-power modes, in every build. The bits
     * survive every reset but a power-on one, so an older image or a debug
     * session that set them would otherwise keep the part from really
     * sleeping (measured: 0.3 mA switched off, against ~3 uA) until the
     * battery was disconnected. A CubeIDE session keeps the loop awake anyway,
     * see plat_sleep_until(). */
    debug_low_power_off();

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
    pvd_off();
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

#if BSP_ENABLE_STOP2
/** Fraction of a millisecond carried between Stop 2 sleeps, in LSE ticks * 1000. */
static uint32_t s_stop_carry;

/**
 * Stop 2 for up to @p ms, interrupts masked by the caller. LPTIM1 ends it, or
 * an EXTI line (button, VBUS, reader) does sooner. SysTick does not run in
 * Stop 2, so the time slept, measured on LPTIM1, is added to HAL's tick.
 */
static bool stop2_for(uint32_t ms)
{
    uint32_t start, elapsed, scaled;

    if (ms > BSP_STOP2_MAX_MS) {
        ms = BSP_STOP2_MAX_MS;
    }
    start = bsp_lptim_count();
    if (!bsp_lptim_wake_in((ms * BSP_LSE_HZ) / 1000u)) {
        return false;   /* nothing would end the sleep: use Sleep mode */
    }

    debug_low_power_off();   /* in case a probe set them since boot */
    HAL_SuspendTick();
    HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
    /* Awake again, on MSI at the range it had (STOPWUCK = MSI), which is the
     * scanning clock: the PLL is never on outside USB sessions. */
    HAL_ResumeTick();

    elapsed = (bsp_lptim_count() - start) & 0xFFFFu;
    scaled = (elapsed * 1000u) + s_stop_carry;
    uwTick += scaled / BSP_LSE_HZ;
    s_stop_carry = scaled % BSP_LSE_HZ;
    return true;
}
#endif

void plat_sleep_until(uint32_t wake_ms, bool deep, plat_idle_pred_t still_idle)
{
    uint32_t primask;
    int32_t ahead;

    bsp_input_rearm();

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
    /* An edge the loop has not seen, from before bsp_input_rearm(): its event
     * was not posted, so the predicate cannot know. */
    if (bsp_input_changed()) {
        sleep_release(primask);
        return;
    }

    ahead = (int32_t)(wake_ms - HAL_GetTick());
#if BSP_ENABLE_STOP2
    if (deep && ahead >= (int32_t)BSP_STOP2_MIN_MS && bsp_lptim_ok() && stop2_for((uint32_t)ahead)) {
        sleep_release(primask);
        return;
    }
#else
    (void)deep;
#endif
    if (ahead <= 0) {
        sleep_release(primask);
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
    pvd_off();
    debug_low_power_off();

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
