/**
 * @file    bsp_board.h
 * @brief   Level 1 (HAL) — the board. Pins, peripheral instances, clocks.
 *
 * Everything device-specific is here, so retargeting to another STM32 or
 * another PCB revision is a change to this file plus the .c files that use it.
 * Nothing in App/ includes this.
 *
 * Target: STM32L432KCU6, UFQFPN32, 256 kB flash / 64 kB SRAM.
 *
 *  Pin   Signal            Function
 *  ----  ----------------  ---------------------------------------------------
 *  PC14  LSE_IN            32.768 kHz crystal. Required: the RTC and both
 *  PC15  LSE_OUT           LPTIMs must keep counting through Stop 2.
 *  PA0   PWR_BTN           WKUP1, EXTI0. The only Standby wake source.
 *  PA1   RF_DATA           TIM2_CH2 input capture, both edges, DMA.
 *  PA2   LED_GREEN         Active high.
 *  PA3   LED_RED           Active high.
 *  PA4   VIB_EN            Vibration motor driver enable.
 *  PA5   RF_PWR_EN         Load switch for the 125 kHz front end.
 *  PA6   TOUCH_PWR_EN      Load switch for the capacitive touch IC.
 *  PA7   BATT_SENSE        ADC1_IN12, 4.7M/4.7M divider off the cell.
 *  PA8   RF_CARRIER        TIM1_CH1 PWM, 125 kHz, 50 %.
 *  PA9   USB_VBUS          VBUS detect, EXTI9, both edges.
 *  PA10  TOUCH_RESET       Pulsed to make the touch IC re-calibrate.
 *  PA11  USB_DM            Fixed function.
 *  PA12  USB_DP            Fixed function.
 *  PA13  SWDIO             Debug. Left configured so the part stays attachable.
 *  PA14  SWCLK             Debug.
 *  PA15  --                Spare.
 *  PB0   --                Spare.
 *  PB1   TOUCH_INT         Card-presence interrupt from the touch IC, EXTI1.
 *  PB3   --                Spare.
 *  PB7   PVD_IN            Same node as PA7; the PVD compares it to VREFINT.
 *  PH3   BOOT0             Strapped low.
 */
#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "stm32l4xx_hal.h"

/* ------------------------------------------------------------------------ */
/* GPIO                                                                     */
/* ------------------------------------------------------------------------ */

#define PIN_PWR_BTN         GPIO_PIN_0
#define PORT_PWR_BTN        GPIOA

#define PIN_RF_DATA         GPIO_PIN_1
#define PORT_RF_DATA        GPIOA

#define PIN_LED_GREEN       GPIO_PIN_2
#define PORT_LED_GREEN      GPIOA

#define PIN_LED_RED         GPIO_PIN_3
#define PORT_LED_RED        GPIOA

#define PIN_VIB_EN          GPIO_PIN_4
#define PORT_VIB_EN         GPIOA

#define PIN_RF_PWR_EN       GPIO_PIN_5
#define PORT_RF_PWR_EN      GPIOA

#define PIN_TOUCH_PWR_EN    GPIO_PIN_6
#define PORT_TOUCH_PWR_EN   GPIOA

#define PIN_BATT_SENSE      GPIO_PIN_7
#define PORT_BATT_SENSE     GPIOA

#define PIN_RF_CARRIER      GPIO_PIN_8
#define PORT_RF_CARRIER     GPIOA

#define PIN_USB_VBUS        GPIO_PIN_9
#define PORT_USB_VBUS       GPIOA

#define PIN_TOUCH_RESET     GPIO_PIN_10
#define PORT_TOUCH_RESET    GPIOA

#define PIN_USB_DM          GPIO_PIN_11
#define PIN_USB_DP          GPIO_PIN_12
#define PORT_USB            GPIOA

#define PIN_TOUCH_INT       GPIO_PIN_1
#define PORT_TOUCH_INT      GPIOB

#define PIN_PVD_IN          GPIO_PIN_7
#define PORT_PVD_IN         GPIOB

/** The touch IC signals a detection with a falling edge and holds it low. */
#define TOUCH_INT_ACTIVE_LEVEL   GPIO_PIN_RESET

/* ------------------------------------------------------------------------ */
/* Clocks                                                                   */
/* ------------------------------------------------------------------------ */

/** MSI range while scanning cards. 4 MHz is the slowest that still decodes a
 *  capture inside the inter-frame gap, and it runs at flash latency 0. */
#define BSP_MSI_RANGE_RUN       RCC_MSIRANGE_6      /*  4 MHz */
#define BSP_SYSCLK_RUN_HZ       4000000u

/** MSI range while USB is enumerated. USB FS needs the core fast enough to
 *  turn packets around; 24 MHz has margin and costs only the USB session. */
#define BSP_MSI_RANGE_USB       RCC_MSIRANGE_9      /* 24 MHz */
#define BSP_SYSCLK_USB_HZ       24000000u

#define BSP_LSE_HZ              32768u

/* ------------------------------------------------------------------------ */
/* Timers                                                                   */
/* ------------------------------------------------------------------------ */

/** Carrier frequency driven into the antenna tank; fixed by EM4100. */
#define BSP_RF_CARRIER_HZ       125000u

/** Time for the tank to ring up and the tag to power from the field before
 *  a capture is worth taking. */
#define BSP_RF_SETTLE_MS        5u

/** 125 kHz carrier. TIM1_CH1 on PA8, AF1. */
#define BSP_CARRIER_TIM         TIM1
#define BSP_CARRIER_CHANNEL     TIM_CHANNEL_1
#define BSP_CARRIER_AF          GPIO_AF1_TIM1

/** Envelope capture. TIM2 is the only 32-bit timer on this part, which is why
 *  it gets this job: a 16-bit counter at 1 MHz wraps every 65 ms, inside the
 *  100 ms read window, and the decoder would have to unwrap it. */
#define BSP_CAPTURE_TIM         TIM2
#define BSP_CAPTURE_CHANNEL     TIM_CHANNEL_2
#define BSP_CAPTURE_AF          GPIO_AF1_TIM2
#define BSP_CAPTURE_HZ          1000000u
#define BSP_CAPTURE_DMA_CH      DMA1_Channel7
#define BSP_CAPTURE_DMA_REQ     DMA_REQUEST_4
#define BSP_CAPTURE_DMA_IRQ     DMA1_Channel7_IRQn

/** Inactivity timer. /128 from the LSE gives 256 Hz, so three minutes is
 *  46 080 counts and fits the 16-bit ARR with room to spare. */
#define BSP_INACT_LPTIM         LPTIM1
#define BSP_INACT_PRESCALER     LPTIM_PRESCALER_DIV128
#define BSP_INACT_TICK_HZ       (BSP_LSE_HZ / 128u)     /* 256 Hz */

/** Short one-shot timer, un-prescaled: 30.5 us per tick, up to 2 s. */
#define BSP_DELAY_LPTIM         LPTIM2
#define BSP_DELAY_PRESCALER     LPTIM_PRESCALER_DIV1
#define BSP_DELAY_TICK_HZ       BSP_LSE_HZ

/* ------------------------------------------------------------------------ */
/* ADC                                                                      */
/* ------------------------------------------------------------------------ */

#define BSP_BATT_ADC            ADC1
#define BSP_BATT_ADC_CHANNEL    ADC_CHANNEL_12          /* PA7 */

/* ------------------------------------------------------------------------ */
/* Flash storage region                                                     */
/* ------------------------------------------------------------------------ */

/**
 * Data lives in the top 128 kB. The linker script keeps code below this;
 * BSP_FLASH_BASE must match the FLASH length in STM32L432KCUX_FLASH.ld.
 */
#define BSP_FLASH_BASE          0x08020000u
#define BSP_FLASH_SIZE          0x00020000u             /* 128 kB */
#define BSP_FLASH_PAGE_SIZE     2048u
#define BSP_FLASH_FIRST_PAGE    64u                     /* page index in bank 1 */

/* ------------------------------------------------------------------------ */
/* RTC backup registers                                                     */
/* ------------------------------------------------------------------------ */

/** Written once the RTC has been set, so a Standby wake can tell a live
 *  calendar from a cold one. */
#define BSP_BKP_RTC_VALID       RTC_BKP_DR0
#define BSP_BKP_RTC_MAGIC       0x52544301u             /* "RTC" v1 */

/* ------------------------------------------------------------------------ */
/* Interrupt priorities (NVIC group 4: all four bits are pre-emption)        */
/* ------------------------------------------------------------------------ */

#define BSP_PRIO_CAPTURE_DMA    5u   /* tightest deadline: 512-entry buffer  */
#define BSP_PRIO_LPTIM          6u
#define BSP_PRIO_EXTI           7u
#define BSP_PRIO_USB            5u
#define BSP_PRIO_PVD            4u   /* pre-empts everything but a fault     */

#endif /* BSP_BOARD_H */
