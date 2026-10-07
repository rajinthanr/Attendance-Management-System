/**
 * @file    bsp_isr.c
 * @brief   Level 1 (HAL) — peripheral interrupt vectors.
 *
 * Kept out of Core/Src/stm32l4xx_it.c so that regenerating from the .ioc
 * never touches them. Every handler does the same thing: hand the HAL its
 * interrupt and let the HAL callback post an event. No decisions are taken
 * at interrupt priority.
 *
 * Interrupts wake the main loop; it does the rest. USB, the button (EXTI0),
 * VBUS (EXTI9), the reader's wake-up (EXTI1), LPTIM1 (the end of a Stop 2
 * sleep) and the PVD (when BSP_ENABLE_BATTERY_PVD is set).
 */
#include "bsp.h"

/* ---- Power ---- */

void PVD_PVM_IRQHandler(void)
{
    HAL_PWREx_PVD_PVM_IRQHandler();
}

/* ---- EXTI: button, reader, VBUS ---- */

void EXTI0_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(PIN_PWR_BTN);
}

void EXTI1_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(PIN_NFC_IRQ);
}

void EXTI9_5_IRQHandler(void)
{
    /* PB6 (the PN532's IRQ) shares the vector but its line is not armed. */
    HAL_GPIO_EXTI_IRQHandler(PIN_USB_VBUS);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == PIN_NFC_IRQ) {
        bsp_nfc_wake_irq();
    } else if (GPIO_Pin == PIN_PWR_BTN || GPIO_Pin == PIN_USB_VBUS) {
        bsp_input_edge();
    }
}

/* ---- Timers ---- */

void LPTIM1_IRQHandler(void)
{
    bsp_lptim_irq();
}

/* ---- USB ---- */

void USB_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hbsp_pcd);
}
