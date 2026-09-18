/**
 * @file    bsp.h
 * @brief   Level 1 (HAL) — board bring-up and the handles the drivers share.
 *
 * Only main.c and the Bsp/ sources include this. App/ reaches Level 1
 * exclusively through platform_if.h.
 */
#ifndef BSP_H
#define BSP_H

#include "main.h"        /* Error_Handler(), shared with the generated code */
#include "bsp_board.h"
#include "platform_if.h"

/**
 * Bring the board up: clocks, GPIO, RTC, timers, ADC, PVD.
 *
 * Deliberately does not arm any interrupt that could post an event. The
 * application decides when it is ready to receive them.
 */
void bsp_init(void);

/** Per-peripheral init, called by bsp_init() in this order. */
void bsp_clock_init(void);
void bsp_gpio_init(void);
void bsp_time_init(void);
void bsp_rf_init(void);
void bsp_adc_init(void);
void bsp_power_init(void);

/** Switch the core clock between the scanning and USB operating points. */
void bsp_clock_set_usb_speed(bool fast);

/** Re-establish the clock tree after a Stop 2 wake, which resets it to MSI. */
void bsp_clock_restore(void);

/** Handles owned by the Bsp, shared between its .c files and the ISRs. */
extern RTC_HandleTypeDef   hbsp_rtc;
extern LPTIM_HandleTypeDef hbsp_lptim_inact;
extern LPTIM_HandleTypeDef hbsp_lptim_delay;
extern TIM_HandleTypeDef   hbsp_tim_carrier;
extern TIM_HandleTypeDef   hbsp_tim_capture;
extern DMA_HandleTypeDef   hbsp_dma_capture;
extern ADC_HandleTypeDef   hbsp_adc;

#endif /* BSP_H */
