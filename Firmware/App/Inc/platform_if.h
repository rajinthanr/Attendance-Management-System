/**
 * @file    platform_if.h
 * @brief   The contract between Level 2 (logic) and Level 1 (HAL drivers).
 *
 * Level 2 *declares* what it needs; Level 1 *implements* it in Bsp/Src.
 * The dependency arrow therefore points from hardware towards logic, which is
 * what lets App/ compile and run on a host with a stub implementation.
 *
 * Rules enforced by this boundary:
 *   - Nothing below returns a cooked/engineering value. ADC counts stay counts,
 *     inputs are raw pin levels, the RTC returns calendar fields, the reader
 *     returns the bytes the card sent. Every conversion is Level 2's job.
 *   - Nothing below makes a decision. There is no plat_handle_card(); Level 1
 *     only moves bytes and toggles pins.
 *   - Level 1 never calls into Level 2 except through app_event_post().
 *
 * Level 2 samples the inputs from the main loop and keeps time with
 * plat_uptime_ms(). Interrupts wake that loop: the button and VBUS edges
 * (APP_EVT_INPUT_EDGE), the reader's wake-up (APP_EVT_NFC_WAKE), the USB
 * peripheral, and LPTIM1 at the end of a plat_sleep_until(). SysTick keeps
 * the millisecond count while the core is running.
 */
#ifndef PLATFORM_IF_H
#define PLATFORM_IF_H

#include "app_types.h"

/* ======================================================================== */
/* Discrete outputs                                                         */
/* ======================================================================== */

#define PLAT_OUT_LED_GREEN   (1u << 0)
#define PLAT_OUT_LED_RED     (1u << 1)
#define PLAT_OUT_VIBRATION   (1u << 2)

/**
 * Drive the user-facing outputs to exactly @p mask.
 * Bits not present in the mask are turned off. Idempotent.
 */
void plat_out_write(uint32_t mask);

/* ======================================================================== */
/* Discrete inputs (polled, raw, not debounced)                             */
/* ======================================================================== */

/** True while the power button is held down. */
bool plat_button_pressed(void);

/** True while USB VBUS is present. */
bool plat_usb_vbus_present(void);

/* ======================================================================== */
/* Power modes                                                              */
/* ======================================================================== */

/**
 * Predicate the sleep call uses to decide whether sleeping is still correct.
 * Returns true while the caller has nothing to do.
 */
typedef bool (*plat_idle_pred_t)(void);

/**
 * Halt the core until @p wake_ms (plat_uptime_ms() time) or an interrupt,
 * whichever comes first. Level 2 works out @p wake_ms: the soonest moment any
 * of its timers needs the loop.
 *
 * With @p deep set the BSP may use Stop 2, for a sleep long enough to be worth
 * it: SysTick stops, LPTIM1 on the LSE wakes the part at @p wake_ms, and the
 * uptime is advanced by the time spent. The button, VBUS and the reader's
 * wake-up are EXTI lines and wake it early. Level 2 sets @p deep only when
 * nothing needs the fast clock (no USB session). Otherwise, and for short
 * sleeps, it is Sleep mode, from which SysTick wakes it within a millisecond.
 *
 * With a debugger attached it returns at once instead, so the debugger's reads
 * of the dbg_* globals are never made while the core sleeps.
 *
 * @p still_idle is re-tested with interrupts masked, immediately before the
 * core is halted, so an event posted from an interrupt between the caller's
 * check and the sleep instruction cannot be slept through. NULL sleeps
 * unconditionally. A button or VBUS edge since the caller last sampled them
 * also keeps it awake.
 */
void plat_sleep_until(uint32_t wake_ms, bool deep, plat_idle_pred_t still_idle);

/**
 * "Off": STM32 Standby. RAM is lost and the only wake source is the power
 * button on the WKUP pin. Does not return; the part resets into main() on wake.
 */
void plat_sleep_deep(void) __attribute__((noreturn));

/** Enter/leave an interrupt-free section. Nestable. */
void plat_critical_enter(void);
void plat_critical_exit(void);

/** Why we booted. Lets the FSM distinguish a Standby wake from a cold start. */
app_boot_cause_t plat_boot_cause(void);

/* ======================================================================== */
/* Time                                                                     */
/* ======================================================================== */

/** Free-running millisecond counter. Wraps after 49 days; compare by subtraction. */
uint32_t plat_uptime_ms(void);

/* ======================================================================== */
/* Real-time clock                                                          */
/* ======================================================================== */

void plat_rtc_get(app_datetime_t *out);
void plat_rtc_set(const app_datetime_t *dt);

/** True once the RTC has been set at least once (tracked in a backup register). */
bool plat_rtc_is_valid(void);

/* ======================================================================== */
/* NFC reader (ST25R3916, ISO14443-A initiator at 106 kbit/s)                */
/* ======================================================================== */

typedef enum {
    PLAT_NFC_OK = 0,
    PLAT_NFC_TIMEOUT,     /**< No response before the no-response timer expired. */
    PLAT_NFC_COLLISION,   /**< The receiver reported a bit collision. */
    PLAT_NFC_RX_ERROR,    /**< CRC, parity or framing error, or a bad FIFO state. */
    PLAT_NFC_IO_ERROR     /**< SPI failure or the reader did not answer. */
} plat_nfc_status_t;

/** plat_nfc_transceive() flags. */
#define PLAT_NFC_TX_CRC          (1u << 0)  /**< Append CRC_A to the frame. */
#define PLAT_NFC_ANTICOLLISION   (1u << 1)  /**< Bit-oriented anticollision frame. */

/**
 * Reset the reader and configure it as an ISO14443-A initiator, field off.
 *
 * @param supply_3v3  Select the reader's 3.3 V supply mode (VDD <= 3.6 V)
 *                    instead of its 5 V mode. Level 2 decides from the battery.
 * @param chip_id     Receives the raw IC identity register, also on failure.
 * @return true when the chip answered and every step completed.
 */
bool plat_nfc_init(bool supply_3v3, uint8_t *chip_id);

/** Change the supply mode and re-run the reader's regulator adjustment. */
bool plat_nfc_set_supply(bool supply_3v3);

/** Switch the 13.56 MHz field (and the receiver) on or off. */
void plat_nfc_field(bool on);

/** Send REQA and return the two ATQA bytes as received. Field must be on. */
plat_nfc_status_t plat_nfc_reqa(uint8_t atqa[2]);

/**
 * Send @p tx and collect the response.
 *
 * @param flags   PLAT_NFC_TX_CRC and/or PLAT_NFC_ANTICOLLISION.
 * @param rx      Receives the bytes exactly as the reader's FIFO holds them,
 *                CRC bytes included when the card sent them.
 * @param rx_len  Receives the number of bytes stored in @p rx.
 */
plat_nfc_status_t plat_nfc_transceive(const uint8_t *tx, uint8_t tx_len,
                                      uint8_t flags, uint8_t *rx,
                                      uint8_t rx_cap, uint8_t *rx_len);

/** Raw A/D reading of the antenna amplitude (13.02 mVpp per count on RFI). */
bool plat_nfc_measure_amplitude(uint8_t *raw);

/**
 * Enter the reader's wake-up mode (DS12484 §4.2.4): oscillator off, field
 * off, and every @p period_ms the chip briefly drives the antenna on its own
 * RC timer and measures the amplitude. A reading that differs from
 * @p reference by more than @p delta counts raises the IRQ pin, and the BSP
 * posts APP_EVT_NFC_WAKE from the interrupt, once: arm again for the next one.
 *
 * @param reference  The amplitude to compare with, in the chip's wake-up
 *                   measurement's own counts. Level 2 derives it from a
 *                   plat_nfc_measure_amplitude() reading taken with no card in
 *                   the field, corrected by what the wake-up mode reported.
 * @param delta      Counts either side of @p reference that do not wake (1-15).
 * @param period_ms  Wanted measurement interval; the chip has 10-80 ms in
 *                   10 ms steps and 100-800 ms in 100 ms steps, and the BSP
 *                   takes the nearest one not longer than asked.
 * @return true when the reader is in wake-up mode and the interrupt armed.
 */
bool plat_nfc_wakeup_arm(uint8_t reference, uint8_t delta, uint16_t period_ms);

/**
 * Leave wake-up mode for Ready mode (oscillator on, field off), disarming the
 * interrupt. Waits for the oscillator, about a millisecond. Harmless when the
 * reader is not in wake-up mode.
 *
 * @param last_raw  Receives the wake-up mode's own last amplitude reading (the
 *                  value it compared with the reference), or 0 when it made
 *                  none or the reader was not in wake-up mode.
 */
bool plat_nfc_wakeup_disarm(uint8_t *last_raw);

/** Put the reader into its power-down mode. plat_nfc_init() wakes it again. */
void plat_nfc_power_down(void);

/* ======================================================================== */
/* Non-volatile storage (internal flash)                                    */
/* ======================================================================== */

/** Size of one erasable page, in bytes. */
uint32_t plat_flash_page_size(void);

/** Offsets below are relative to the start of the storage region, not 0x08000000. */
bool plat_flash_read(uint32_t offset, void *dst, uint32_t len);

/** Program one 64-bit double-word. @p offset must be 8-byte aligned. */
bool plat_flash_write_dw(uint32_t offset, uint64_t value);

/** Erase the page containing @p offset. */
bool plat_flash_erase(uint32_t offset);

/* ======================================================================== */
/* Battery measurement                                                      */
/* ======================================================================== */

/**
 * Take one battery + VREFINT sample pair. Blocking, about a millisecond.
 * Returns raw counts only.
 */
bool plat_adc_sample(app_adc_sample_t *out);

/* ======================================================================== */
/* USB device                                                               */
/* ======================================================================== */

/** Bring up the USB peripheral and enumerate as a mass-storage device. */
void plat_usb_start(void);

/** Tear USB down and release its clocks. */
void plat_usb_stop(void);

/** True once a host has configured the device (as opposed to a bare charger). */
bool plat_usb_configured(void);

/**
 * True once the host has ejected the drive this session (SCSI START STOP UNIT
 * with START = 0: Eject in Windows Explorer, macOS Finder or a Linux file
 * manager, `eject`, `udisksctl power-off`). Cleared by plat_usb_start().
 */
bool plat_usb_ejected(void);

#endif /* PLATFORM_IF_H */
