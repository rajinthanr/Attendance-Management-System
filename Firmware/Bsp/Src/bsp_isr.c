/**
 * @file    bsp_isr.c
 * @brief   Level 1 (HAL) — peripheral interrupt vectors.
 *
 * Kept out of Core/Src/stm32l4xx_it.c so that regenerating from the .ioc
 * never touches them. Every handler does the same thing: hand the HAL its
 * interrupt and let the HAL callback post an event. No decisions are taken
 * at interrupt priority.
 *
 * Polling mode: the button, VBUS, the reader and the timers are sampled from
 * the main loop, so only USB (and the PVD, when BSP_ENABLE_BATTERY_PVD is set)
 * interrupt. EXTI and LPTIM handlers return here with the move to interrupts.
 */
#include "bsp.h"

/* ---- Power ---- */

void PVD_PVM_IRQHandler(void)
{
    HAL_PWREx_PVD_PVM_IRQHandler();
}

/* ---- USB ---- */

void USB_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hbsp_pcd);
}
