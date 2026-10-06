/**
 * @file    bsp_gpio.c
 * @brief   Level 1 (HAL) — pin configuration and discrete outputs.
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
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_SET);
    HAL_GPIO_WritePin(PORT_PN532_NSS, PIN_PN532_NSS, GPIO_PIN_SET);

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = PIN_LED_GREEN | PIN_LED_RED | PIN_VIB_EN | PIN_PN532_NSS;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = PIN_NFC_NSS;
    HAL_GPIO_Init(PORT_NFC_NSS, &g);

    /* bsp_gpio_init runs after MX_SPI1_Init, so restore the shared SPI pins
     * after parking unused pins as analog. NSS remains software controlled. */
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI1;
    g.Pull = GPIO_NOPULL;
    g.Pin = PIN_NFC_SCK | PIN_NFC_MOSI;
    HAL_GPIO_Init(PORT_NFC_SPI, &g);
    g.Pull = GPIO_PULLDOWN;
    g.Pin = PIN_NFC_MISO;
    HAL_GPIO_Init(PORT_NFC_SPI, &g);

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

    /* ST25R3916 IRQ is active high; the pull-down keeps its idle state
     * defined while the reader is resetting. */
    g.Mode = GPIO_MODE_IT_RISING;
    g.Pull = GPIO_PULLDOWN;
    g.Pin = PIN_NFC_IRQ;
    HAL_GPIO_Init(PORT_NFC_IRQ, &g);

    /* The optional PN532 has its own active-low interrupt on EXTI6. */
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    g.Pin = PIN_PN532_IRQ;
    HAL_GPIO_Init(PORT_PN532_IRQ, &g);

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

    /* The button and VBUS are always live; NFC is armed by the application. */
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
    /* ST25R3916 is powered directly from the battery. */
    (void)on;
}

void plat_touch_irq_enable(bool enable)
{
    if (enable) {
        __HAL_GPIO_EXTI_CLEAR_IT(PIN_NFC_IRQ);
        HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
        HAL_NVIC_EnableIRQ(EXTI1_IRQn);
    } else {
        HAL_NVIC_DisableIRQ(EXTI1_IRQn);
    }
}

void plat_touch_recalibrate(void)
{
    /* No discrete capacitive touch controller is fitted on this PCB. */
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

    case PIN_NFC_IRQ:
        app_event_post(APP_EVT_TOUCH);
        break;

    case PIN_PN532_IRQ:
        /* Backup reader support is not enabled by the application yet. */
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
