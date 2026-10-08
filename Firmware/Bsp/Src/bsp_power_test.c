/**
 * @file    bsp_power_test.c
 * @brief   Level 1 (HAL) — bench build that holds one load at a time steady,
 *          so a multimeter can read it.
 *
 * Built only with BSP_POWER_TEST = 1 (`make power-test`). It replaces the
 * application: main() calls bsp_power_test() after bsp_init() and never
 * reaches app_init(). Normal operation only draws these currents for
 * milliseconds at a time, which a meter cannot read.
 *
 * A short press of the button steps to the next mode. Each mode first blinks
 * green as many times as its number, then holds its load until the next
 * press. Mode 10 is Standby; a press there reboots into mode 1.
 *
 *   1  MCU Stop 2, reader power-down          (µA range)
 *   2  MCU Stop 2, reader wake-up mode 100 ms (µA range)
 *   3  MCU Stop 2, reader Ready, field off    (mA range)
 *   4  MCU Stop 2, reader RF field on         (mA range)
 *   5  MCU Run 4 MHz, reader power-down       (mA range)
 *   6  MCU Sleep 4 MHz (SysTick every 1 ms)   (mA range)
 *   7  Green LED on, MCU Stop 2               (mA range)
 *   8  Red LED on, MCU Stop 2                 (mA range)
 *   9  Vibration motor on, MCU Stop 2         (mA range)
 *  10  Standby (switched off)                 (µA range)
 */
#include "bsp.h"
#include "app_config.h"

#if BSP_POWER_TEST

#define PT_MODES        10u
#define PT_DEBOUNCE_MS  30u

static bool s_nfc_ok;

static bool button_down(void)
{
    return HAL_GPIO_ReadPin(PORT_PWR_BTN, PIN_PWR_BTN) == PWR_BTN_ACTIVE_LEVEL;
}

/** A debounced press and release. */
static bool pressed(void)
{
    if (!button_down()) {
        return false;
    }
    HAL_Delay(PT_DEBOUNCE_MS);
    if (!button_down()) {
        return false;
    }
    while (button_down()) {
    }
    HAL_Delay(PT_DEBOUNCE_MS);
    return true;
}

static void blink(uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        plat_out_write(PLAT_OUT_LED_GREEN);
        HAL_Delay(100u);
        plat_out_write(0u);
        HAL_Delay(250u);
    }
    HAL_Delay(500u);
}

/** Stop 2 until the button is pressed. LPTIM1 and a reader wake-up also end
 *  a Stop 2; those just go back to sleep. */
static void stop2_until_press(void)
{
    for (;;) {
        HAL_SuspendTick();
        HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
        HAL_ResumeTick();
        if (pressed()) {
            return;
        }
    }
}

static void reader_off(void)
{
    uint8_t raw;

    if (s_nfc_ok) {
        (void)plat_nfc_wakeup_disarm(&raw);
        plat_nfc_field(false);
    }
    plat_nfc_power_down();
}

/** Back to Ready mode (oscillator on), from power-down or wake-up mode. */
static bool reader_ready(void)
{
    uint8_t id;

    s_nfc_ok = plat_nfc_init(false, &id);
    return s_nfc_ok;
}

static void error_blink(void)
{
    uint32_t i;

    for (i = 0u; i < 5u; i++) {
        plat_out_write(PLAT_OUT_LED_RED);
        HAL_Delay(100u);
        plat_out_write(0u);
        HAL_Delay(100u);
    }
}

void bsp_power_test(void)
{
    uint32_t mode;
    uint8_t amplitude;

    /* Check the reader answers before stepping through the modes. */
    plat_out_write(0u);
    if (!reader_ready()) {
        error_blink();
    }
    reader_off();

    for (mode = 1u;; mode = (mode % PT_MODES) + 1u) {
        blink(mode);
        switch (mode) {
        case 1u:
            reader_off();
            stop2_until_press();
            break;
        case 2u:
            if (!reader_ready() || !plat_nfc_measure_amplitude(&amplitude) ||
                !plat_nfc_wakeup_arm(amplitude, APP_NFC_WAKE_DELTA_MIN,
                                     (uint16_t)APP_NFC_WAKE_PERIOD_MS)) {
                error_blink();
            }
            stop2_until_press();
            reader_off();
            break;
        case 3u:
            if (!reader_ready()) {
                error_blink();
            }
            stop2_until_press();
            reader_off();
            break;
        case 4u:
            if (!reader_ready()) {
                error_blink();
            }
            plat_nfc_field(true);
            stop2_until_press();
            reader_off();
            break;
        case 5u:
            while (!pressed()) {
            }
            break;
        case 6u:
            do {
                HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
            } while (!pressed());
            break;
        case 7u:
            plat_out_write(PLAT_OUT_LED_GREEN);
            stop2_until_press();
            break;
        case 8u:
            plat_out_write(PLAT_OUT_LED_RED);
            stop2_until_press();
            break;
        case 9u:
            plat_out_write(PLAT_OUT_VIBRATION);
            stop2_until_press();
            break;
        default:
            plat_out_write(0u);
            plat_nfc_power_down();
            plat_sleep_deep();   /* a press wakes it through reset: mode 1 */
            break;
        }
        plat_out_write(0u);
    }
}

#endif /* BSP_POWER_TEST */
