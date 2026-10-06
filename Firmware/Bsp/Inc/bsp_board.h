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
 *  PC14  LSE_IN            32.768 kHz crystal. Keeps the RTC, and with it
 *  PC15  LSE_OUT           the CSV timestamps, running while the unit is off.
 *  PA0   PWR_BTN           WKUP1, polled input. The only Standby wake source.
 *  PA1   --                Spare.
 *  PA2   LED_GREEN         Active high.
 *  PA3   LED_RED           Active high.
 *  PA4   VIB_EN            Vibration motor driver enable.
 *  PA5   --                Spare.
 *  PA6   --                Spare.
 *  PA7   BATT_SENSE        ADC1_IN12, 4.7M/2.7M divider off the cell.
 *  PA8   --                Spare.
 *  PA9   USB_VBUS          VBUS detect through R18/R19, polled input.
 *  PA10  --                Spare.
 *  PA11  USB_DM            Fixed function.
 *  PA12  USB_DP            Fixed function.
 *  PA13  SWDIO             Debug. Left configured so the part stays attachable.
 *  PA14  SWCLK             Debug.
 *  PA15  PN532_NSS         Backup reader chip select, active low.
 *  PB0   NFC_NSS           ST25R3916 chip select, active low.
 *  PB1   NFC_IRQ           ST25R3916 interrupt, active high, polled input.
 *  PB3   NFC_SCK           Shared SPI1 clock, AF5.
 *  PB4   NFC_MISO          Shared SPI1 input, AF5, pull-down.
 *  PB5   NFC_MOSI          Shared SPI1 output, AF5.
 *  PB6   PN532_IRQ         Backup reader interrupt, active low. Unused.
 *  PB7   PVD_IN            Same node as PA7; the PVD compares it to VREFINT.
 *  PH3   BOOT0             Strapped low.
 */
#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "stm32l4xx_hal.h"

/* The PVD on PVD_IN raises an interrupt, and the firmware currently polls the
 * battery through the ADC instead. Re-enable with the move to interrupts. */
#define BSP_ENABLE_BATTERY_PVD 0u

/* ------------------------------------------------------------------------ */
/* GPIO                                                                     */
/* ------------------------------------------------------------------------ */

#define PIN_PWR_BTN         GPIO_PIN_0
#define PORT_PWR_BTN        GPIOA

#define PIN_LED_GREEN       GPIO_PIN_2
#define PORT_LED_GREEN      GPIOA

#define PIN_LED_RED         GPIO_PIN_3
#define PORT_LED_RED        GPIOA

#define PIN_VIB_EN          GPIO_PIN_4
#define PORT_VIB_EN         GPIOA

#define PIN_BATT_SENSE      GPIO_PIN_7
#define PORT_BATT_SENSE     GPIOA

#define PIN_USB_VBUS        GPIO_PIN_9
#define PORT_USB_VBUS       GPIOA

#define PIN_USB_DM          GPIO_PIN_11
#define PIN_USB_DP          GPIO_PIN_12
#define PORT_USB            GPIOA

#define PIN_PN532_NSS       GPIO_PIN_15
#define PORT_PN532_NSS      GPIOA

#define PIN_NFC_NSS         GPIO_PIN_0
#define PORT_NFC_NSS        GPIOB
#define PIN_NFC_IRQ         GPIO_PIN_1
#define PORT_NFC_IRQ        GPIOB
#define PIN_NFC_SCK         GPIO_PIN_3
#define PIN_NFC_MISO        GPIO_PIN_4
#define PIN_NFC_MOSI        GPIO_PIN_5
#define PORT_NFC_SPI        GPIOB
#define PIN_PN532_IRQ       GPIO_PIN_6
#define PORT_PN532_IRQ      GPIOB

#define PIN_PVD_IN          GPIO_PIN_7
#define PORT_PVD_IN         GPIOB

/** The power button shorts PA0 to ground. */
#define PWR_BTN_ACTIVE_LEVEL     GPIO_PIN_RESET

/** ST25R3916 IRQ is active high. */
#define NFC_IRQ_ACTIVE_LEVEL     GPIO_PIN_SET

/* ------------------------------------------------------------------------ */
/* Clocks                                                                   */
/* ------------------------------------------------------------------------ */

/** MSI range while scanning cards. 4 MHz runs at flash latency 0 and keeps
 *  SPI1 well inside the reader's 10 MHz limit. */
#define BSP_MSI_RANGE_RUN       RCC_MSIRANGE_6      /*  4 MHz */
#define BSP_SYSCLK_RUN_HZ       4000000u

/** MSI range while USB is enumerated. USB FS needs the core fast enough to
 *  turn packets around; 24 MHz has margin and costs only the USB session. */
#define BSP_MSI_RANGE_USB       RCC_MSIRANGE_9      /* 24 MHz */
#define BSP_SYSCLK_USB_HZ       24000000u

#define BSP_LSE_HZ              32768u

/* ------------------------------------------------------------------------ */
/* NFC reader (ST25R3916 on SPI1)                                           */
/* ------------------------------------------------------------------------ */

#define BSP_NFC_SPI             SPI1

/** SPI1 runs from PCLK2: /8 gives 500 kHz at 4 MHz and 3 MHz at the 24 MHz
 *  USB operating point, both well inside the reader's 10 MHz. */
#define BSP_NFC_SPI_PRESCALER   SPI_BAUDRATEPRESCALER_8
#define BSP_NFC_SPI_TIMEOUT_MS  10u

/** No-response timer, in 64/fc (4.72 us) steps: 212 is 1 ms, ten times the
 *  ~90 us an ISO14443-A card takes to answer REQA, anticollision or SELECT. */
#define BSP_NFC_NRT_64FC        212u

/** Backstops for waits the chip normally ends itself (DS12484 §4.4). */
#define BSP_NFC_OSC_TIMEOUT_MS      50u   /* crystal start-up            */
#define BSP_NFC_ADJUST_TIMEOUT_MS   10u   /* Adjust regulators, 5 ms max */
#define BSP_NFC_MEASURE_TIMEOUT_MS  2u    /* Measure amplitude, 25 us max */
#define BSP_NFC_RX_TIMEOUT_MS       10u   /* the NRT normally ends it at 1 ms */

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

#define BSP_PRIO_USB            5u
#define BSP_PRIO_PVD            4u   /* pre-empts everything but a fault     */

#endif /* BSP_BOARD_H */
