/**
 * @file    app_debug.h
 * @brief   Globals for STM32CubeIDE Live Expressions.
 *
 * Defined in Core/Src/main.c (USER CODE PV block) on the target, and in
 * Tests/host_platform.c for the host build. The state machine refreshes them
 * every pass of the main loop (the clock once a second); bsp_nfc.c writes the
 * two reader interrupt values. Nothing in the firmware reads them back except
 * the two dbg_set_time* variables, which exist to be written from the
 * debugger.
 */
#ifndef APP_DEBUG_H
#define APP_DEBUG_H

#include "app_types.h"

/* ---- Battery ------------------------------------------------------------ */
extern volatile uint32_t dbg_battery_mv;      /**< Last cell voltage, mV. */
extern volatile uint8_t  dbg_battery_state;   /**< 0 OK, 1 warn, 2 critical. */
extern volatile uint16_t dbg_battery_counts;  /**< Raw ADC counts at PA7. */
extern volatile uint32_t dbg_battery_samples; /**< Sampling attempts since boot. */
extern volatile uint8_t  dbg_battery_error;   /**< Last attempt: 0 OK, 1 ADC failed (see
                                                   dbg_adc_error), 2 VREFINT read 0,
                                                   3 VDDA out of range, 4 input saturated,
                                                   5 above 4.5 V. */
extern volatile uint32_t dbg_battery_raw_mv;  /**< Computed even when rejected. */
extern volatile uint32_t dbg_vdda_mv;         /**< VDDA from VREFINT; ~3300 expected. */
extern volatile uint16_t dbg_vrefint_counts;  /**< Raw VREFINT reading; ~1500 at 3.3 V. */
extern volatile uint16_t dbg_vrefint_cal;     /**< Factory VREFINT_CAL; ~1650. */
extern volatile uint8_t  dbg_adc_error;       /**< ADC step that failed: 0 none, 1 init or
                                                   calibration, 2 PA7, 3 VREFINT. */

/* ---- Last card ---------------------------------------------------------- */
extern volatile uint32_t dbg_card_id;         /**< ID as logged (CSV column 3). */
extern volatile uint8_t  dbg_card_uid[10];    /**< UID bytes as the card sent them. */
extern volatile uint8_t  dbg_card_uid_len;    /**< 4, 7 or 10; 0 before any card. */
extern volatile uint8_t  dbg_card_atqa[2];
extern volatile uint8_t  dbg_card_sak;
extern volatile uint32_t dbg_card_count;      /**< Cards presented since boot. */
extern volatile uint8_t  dbg_scan_result;     /**< app_scan_result_t: 1 accepted,
                                                   2 duplicate, 3 unknown, 4 full. */

/* ---- Button ------------------------------------------------------------- */
extern volatile bool     dbg_button_down;     /**< Debounced level. */
extern volatile uint32_t dbg_button_short_count;
extern volatile uint32_t dbg_button_long_count;

/* ---- Reader ------------------------------------------------------------- */
extern volatile bool     dbg_nfc_ready;       /**< Reader initialised. */
extern volatile uint8_t  dbg_nfc_chip_id;     /**< 0x2A expected (ST25R3916 rev 3.1). */
extern volatile bool     dbg_nfc_supply_3v3;  /**< Reader in its 3.3 V supply mode. */
extern volatile uint8_t  dbg_nfc_amplitude;   /**< Antenna amplitude; ~190 is ideal. */
extern volatile uint8_t  dbg_nfc_last_status; /**< iso14443a_status_t of the last poll:
                                                   0 card, 1 none, 2 collision,
                                                   3 protocol, 4 reader fault. */
extern volatile uint32_t dbg_nfc_polls;
extern volatile uint32_t dbg_nfc_errors;
extern volatile uint32_t dbg_nfc_collisions;
extern volatile uint32_t dbg_nfc_last_irq;    /**< Reader IRQ flags of the last exchange:
                                                   main | timer << 8 | error << 16. */
extern volatile uint32_t dbg_nfc_irq_pin_misses; /**< Non-zero: IRQ line not reaching PB1. */

/* ---- System ------------------------------------------------------------- */
extern volatile uint8_t  dbg_state;           /**< app_state_t: 0 idle, 1 USB, 2 shutdown. */
extern volatile uint8_t  dbg_boot_cause;      /**< app_boot_cause_t. */
extern volatile uint32_t dbg_uptime_ms;
extern volatile bool     dbg_vbus;            /**< Debounced VBUS. */
extern volatile bool     dbg_usb_host;        /**< A host enumerated the device. */
extern volatile uint32_t dbg_records_ram;     /**< Waiting in RAM. */
extern volatile uint32_t dbg_records_flash;   /**< In the log (exported as CSV). */
extern volatile uint32_t dbg_records_free;    /**< Log slots left. */
extern volatile uint32_t dbg_students;        /**< Provisioned list size; 0 = none. */
extern volatile app_datetime_t dbg_now;       /**< RTC, refreshed once a second. */

/* ---- Written from the debugger ------------------------------------------ */
/** Fill in dbg_set_time, then set dbg_set_time_request to 1. The firmware
 *  sets the RTC and clears the request; an invalid date is ignored. */
extern volatile app_datetime_t dbg_set_time;
extern volatile bool     dbg_set_time_request;

#endif /* APP_DEBUG_H */
