/**
 * @file    bsp_gpio.c
 * @brief   Level 1 (HAL) — pin configuration, discrete outputs, touch IC.
 *
 * Unused pins are left as analog inputs, which is the lowest-leakage state on
 * an L4 and is worth real microamps in Stop 2 across sixteen spare pins.
 */
#include "bsp.h"
#include "app_events.h"

void bsp_gpio_init(void)
{
    GPIO_InitTypeDef g = { 0 };

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    /* GPIOC is deliberately left unclocked: its only pins on this package are
     * PC14/PC15, which belong to the LSE oscillator and are configured by the
     * RCC, not by us. */

    /* Everything analog first, then override the pins we actually use. This
     * catches spare pins without having to enumerate them. */
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    g.Pin = GPIO_PIN_All & ~(uint32_t)(GPIO_PIN_13 | GPIO_PIN_14);  /* keep SWD */
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_All;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_Init(GPIOH, &g);

    /* Outputs, all starting off. Push-pull; the LEDs and the motor driver are
     * all active high. */
    HAL_GPIO_WritePin(PORT_LED_GREEN, PIN_LED_GREEN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_LED_RED, PIN_LED_RED, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_VIB_EN, PIN_VIB_EN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_RF_PWR_EN, PIN_RF_PWR_EN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_TOUCH_PWR_EN, PIN_TOUCH_PWR_EN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_TOUCH_RESET, PIN_TOUCH_RESET, GPIO_PIN_SET);

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = PIN_LED_GREEN | PIN_LED_RED | PIN_VIB_EN |
            PIN_RF_PWR_EN | PIN_TOUCH_PWR_EN | PIN_TOUCH_RESET;
    HAL_GPIO_Init(GPIOA, &g);

    /* Power button: WKUP1 wakes from Standby, EXTI0 catches a press while
     * running. Pulled up, so the button shorts to ground. */
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    g.Pin = PIN_PWR_BTN;
    HAL_GPIO_Init(PORT_PWR_BTN, &g);

    /* VBUS: both edges, so attach and detach are distinguishable. No pull;
     * the divider on the VBUS net defines the level. */
    g.Mode = GPIO_MODE_IT_RISING_FALLING;
    g.Pull = GPIO_NOPULL;
    g.Pin = PIN_USB_VBUS;
    HAL_GPIO_Init(PORT_USB_VBUS, &g);

    /* Touch interrupt: open-drain output on the touch IC, so it needs a pull
     * up here to define the idle level. */
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    g.Pin = PIN_TOUCH_INT;
    HAL_GPIO_Init(PORT_TOUCH_INT, &g);

    /* Battery divider node feeds the ADC and the PVD comparator. */
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    g.Pin = PIN_BATT_SENSE;
    HAL_GPIO_Init(PORT_BATT_SENSE, &g);
    g.Pin = PIN_PVD_IN;
    HAL_GPIO_Init(PORT_PVD_IN, &g);

    HAL_NVIC_SetPriority(EXTI0_IRQn, BSP_PRIO_EXTI, 0u);
    HAL_NVIC_SetPriority(EXTI1_IRQn, BSP_PRIO_EXTI, 0u);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, BSP_PRIO_EXTI, 0u);

    /* The button and VBUS are always live; touch is armed by the application. */
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

/* ------------------------------------------------------------------------ */
/* platform_if: discrete outputs                                            */
/* ------------------------------------------------------------------------ */

void plat_out_write(uint32_t mask)
{
    HAL_GPIO_WritePin(PORT_LED_GREEN, PIN_LED_GREEN,
                      (mask & PLAT_OUT_LED_GREEN) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_LED_RED, PIN_LED_RED,
                      (mask & PLAT_OUT_LED_RED) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORT_VIB_EN, PIN_VIB_EN,
                      (mask & PLAT_OUT_VIBRATION) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ------------------------------------------------------------------------ */
/* platform_if: touch IC                                                    */
/* ------------------------------------------------------------------------ */

void plat_touch_power(bool on)
{
    HAL_GPIO_WritePin(PORT_TOUCH_PWR_EN, PIN_TOUCH_PWR_EN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void plat_touch_irq_enable(bool enable)
{
    if (enable) {
        /* Clear anything latched while the carrier was running, or the pad
         * would fire immediately on re-arming. */
        __HAL_GPIO_EXTI_CLEAR_IT(PIN_TOUCH_INT);
        HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
        HAL_NVIC_EnableIRQ(EXTI1_IRQn);
    } else {
        HAL_NVIC_DisableIRQ(EXTI1_IRQn);
    }
}

void plat_touch_recalibrate(void)
{
    /* Active-low reset. The datasheet minimum is a few microseconds; the
     * self-calibration that follows takes the IC tens of milliseconds, which
     * overlaps the rest of start-up. */
    HAL_GPIO_WritePin(PORT_TOUCH_RESET, PIN_TOUCH_RESET, GPIO_PIN_RESET);
    HAL_Delay(1u);
    HAL_GPIO_WritePin(PORT_TOUCH_RESET, PIN_TOUCH_RESET, GPIO_PIN_SET);
}

/* ------------------------------------------------------------------------ */
/* platform_if: USB presence                                                */
/* ------------------------------------------------------------------------ */

bool plat_usb_vbus_present(void)
{
    return (HAL_GPIO_ReadPin(PORT_USB_VBUS, PIN_USB_VBUS) == GPIO_PIN_SET);
}

/* ------------------------------------------------------------------------ */
/* EXTI callback: the single place GPIO interrupts become events            */
/* ------------------------------------------------------------------------ */

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    switch (pin) {
    case PIN_PWR_BTN:
        app_event_post(APP_EVT_BUTTON);
        break;

    case PIN_TOUCH_INT:
        app_event_post(APP_EVT_TOUCH);
        break;

    case PIN_USB_VBUS:
        /* Both edges share one line, so the level decides which it was. */
        app_event_post(plat_usb_vbus_present() ? APP_EVT_USB_ATTACH
                                               : APP_EVT_USB_DETACH);
        break;

    default:
        break;
    }
}
