/**
 * @file    bsp_isr.c
 * @brief   Level 1 (HAL) — peripheral interrupt vectors.
 *
 * Kept out of Core/Src/stm32l4xx_it.c so that regenerating from the .ioc
 * never touches them. Every handler does the same thing: hand the HAL its
 * interrupt and let the HAL callback post an event. No decisions are taken
 * at interrupt priority.
 */
#include "bsp.h"

extern PCD_HandleTypeDef hbsp_pcd;

/* ---- EXTI ---- */

void EXTI0_IRQHandler(void)          /* power button */
{
    HAL_GPIO_EXTI_IRQHandler(PIN_PWR_BTN);
}

void EXTI1_IRQHandler(void)          /* touch IC */
{
    HAL_GPIO_EXTI_IRQHandler(PIN_TOUCH_INT);
}

void EXTI9_5_IRQHandler(void)        /* USB VBUS */
{
    HAL_GPIO_EXTI_IRQHandler(PIN_USB_VBUS);
}

/* ---- Timers ---- */

void LPTIM1_IRQHandler(void)         /* 3-minute inactivity */
{
    HAL_LPTIM_IRQHandler(&hbsp_lptim_inact);
}

void LPTIM2_IRQHandler(void)         /* short one-shot delays */
{
    HAL_LPTIM_IRQHandler(&hbsp_lptim_delay);
}

/* ---- Capture DMA ---- */

void DMA1_Channel7_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hbsp_dma_capture);
}

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
