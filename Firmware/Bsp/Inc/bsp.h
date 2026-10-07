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
 * Bring the board up: clocks, GPIO, RTC, ADC, power.
 *
 * Deliberately does not arm any interrupt that could post an event. The
 * application decides when it is ready to receive them.
 */
void bsp_init(void);

/** First call in main(), before HAL_Init(): set VTOR to our flash and clear a
 *  stale FLASH_SR.PEMPTY left by the SWD flash loader (bsp_power.c). */
void bsp_early_init(void);

/** Per-peripheral init, called by bsp_init() in this order. */
void bsp_clock_init(void);
void bsp_gpio_init(void);
void bsp_time_init(void);
void bsp_adc_init(void);
void bsp_power_init(void);

/** Switch the core clock between the scanning and USB operating points. */
void bsp_clock_set_usb_speed(bool fast);

/** EXTI1 fired while the reader was in wake-up mode. Interrupt context. */
void bsp_nfc_wake_irq(void);

/** The button or VBUS changed (EXTI0 / EXTI9). Interrupt context. */
void bsp_input_edge(void);

/** Before a sleep: let the next edge post an event again. */
void bsp_input_rearm(void);

/** True when the button or VBUS level differs from what the loop last read. */
bool bsp_input_changed(void);

/** LPTIM1 counter (32768 Hz, wraps at 2 s), read safely. */
uint32_t bsp_lptim_count(void);

/** LPTIM1 interrupt: a Stop 2 sleep has reached its end. */
void bsp_lptim_irq(void);

/** Arm LPTIM1 to wake the part @p ticks from now (at most 0xFFFF). False
 *  when the timer did not take the write (no LSE): do not enter Stop 2. */
bool bsp_lptim_wake_in(uint32_t ticks);

/** LPTIM1 started and answers; false means no Stop 2 (see dbg_lptim_fault). */
bool bsp_lptim_ok(void);

/** Handles owned by the Bsp, shared between its .c files and the ISRs. */
extern RTC_HandleTypeDef   hbsp_rtc;
extern ADC_HandleTypeDef   hbsp_adc;
extern SPI_HandleTypeDef   hbsp_spi;
extern PCD_HandleTypeDef   hbsp_pcd;

#endif /* BSP_H */
