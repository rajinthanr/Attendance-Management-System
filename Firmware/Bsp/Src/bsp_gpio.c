/**
 * @file    bsp_gpio.c
 * @brief   Level 1 (HAL) — pin configuration and discrete outputs.
 *
 * Unused pins are left as analog inputs, which is the lowest-leakage state on
 * an L4 and is worth real microamps across sixteen spare pins.
 *
 * The button (EXTI0) and VBUS (EXTI9) interrupt on both edges, only to wake
 * the main loop, which samples their levels and debounces them; see
 * bsp_input_edge(). The reader's IRQ is set up on EXTI1 with the line masked:
 * bsp_nfc.c polls the pin during exchanges and unmasks the line only while
 * the reader is in wake-up mode.
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

    /* MX_GPIO_Init() armed EXTI on the inputs, and HAL_GPIO_Init() never
     * disarms a line, so release them before reconfiguring. */
    HAL_GPIO_DeInit(GPIOA, PIN_PWR_BTN | PIN_USB_VBUS);
    HAL_GPIO_DeInit(GPIOB, PIN_NFC_IRQ | PIN_PN532_IRQ);

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

    /* Power button: WKUP1 wakes from Standby; an edge wakes the loop while
     * running. R8 pulls it up externally as well, and the button shorts it to
     * ground. */
    g.Mode = GPIO_MODE_IT_RISING_FALLING;
    g.Pull = GPIO_PULLUP;
    g.Pin = PIN_PWR_BTN;
    HAL_GPIO_Init(PORT_PWR_BTN, &g);

    /* VBUS: no pull; the R18/R19 divider on the VBUS net defines the level. */
    g.Pull = GPIO_NOPULL;
    g.Pin = PIN_USB_VBUS;
    HAL_GPIO_Init(PORT_USB_VBUS, &g);
    __HAL_GPIO_EXTI_CLEAR_IT(PIN_PWR_BTN | PIN_USB_VBUS);
    HAL_NVIC_SetPriority(EXTI0_IRQn, BSP_PRIO_INPUT, 0u);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, BSP_PRIO_INPUT, 0u);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
    g.Mode = GPIO_MODE_INPUT;

    /* ST25R3916 IRQ is active high; the pull-down keeps its idle state
     * defined while the reader is resetting. HAL_GPIO_Init() unmasks the
     * line; mask it again, since bsp_init() arms nothing that posts events. */
    g.Mode = GPIO_MODE_IT_RISING;
    g.Pull = GPIO_PULLDOWN;
    g.Pin = PIN_NFC_IRQ;
    HAL_GPIO_Init(PORT_NFC_IRQ, &g);
    CLEAR_BIT(EXTI->IMR1, PIN_NFC_IRQ);
    __HAL_GPIO_EXTI_CLEAR_IT(PIN_NFC_IRQ);
    HAL_NVIC_SetPriority(EXTI1_IRQn, BSP_PRIO_NFC, 0u);
    HAL_NVIC_EnableIRQ(EXTI1_IRQn);
    g.Mode = GPIO_MODE_INPUT;

    /* The optional PN532 drives its active-low interrupt here. */
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
/* platform_if: discrete inputs                                             */
/* ------------------------------------------------------------------------ */

/* The levels the main loop last saw, and whether an edge has already woken
 * it since the last sleep. */
static volatile bool s_seen_button;
static volatile bool s_seen_vbus;
static volatile bool s_edge_posted;

bool plat_button_pressed(void)
{
    s_seen_button = (HAL_GPIO_ReadPin(PORT_PWR_BTN, PIN_PWR_BTN) == PWR_BTN_ACTIVE_LEVEL);
    return s_seen_button;
}

bool plat_usb_vbus_present(void)
{
    s_seen_vbus = (HAL_GPIO_ReadPin(PORT_USB_VBUS, PIN_USB_VBUS) == GPIO_PIN_SET);
    return s_seen_vbus;
}

void bsp_input_edge(void)
{
    /* A bouncing contact makes dozens of edges; one event per sleep is all
     * the loop needs, since it samples the levels itself. */
    if (!s_edge_posted) {
        s_edge_posted = true;
        app_event_post(APP_EVT_INPUT_EDGE);
    }
}

void bsp_input_rearm(void)
{
    s_edge_posted = false;
}

bool bsp_input_changed(void)
{
    bool button = (HAL_GPIO_ReadPin(PORT_PWR_BTN, PIN_PWR_BTN) == PWR_BTN_ACTIVE_LEVEL);
    bool vbus = (HAL_GPIO_ReadPin(PORT_USB_VBUS, PIN_USB_VBUS) == GPIO_PIN_SET);

    return (button != s_seen_button) || (vbus != s_seen_vbus);
}
