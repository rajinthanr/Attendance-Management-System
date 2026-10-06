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
 * The firmware currently runs in polling mode: Level 2 samples every input
 * from the main loop and keeps time with plat_uptime_ms(). The only interrupts
 * in use are SysTick (behind plat_uptime_ms) and the USB peripheral.
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
 * Core halted until the next interrupt (STM32 Sleep mode). SysTick fires every
 * millisecond, so in polling mode this paces the main loop at 1 kHz.
 *
 * @p still_idle is re-tested with interrupts masked, immediately before the
 * core is halted, so an event posted from an interrupt between the caller's
 * check and the sleep instruction cannot be slept through. NULL sleeps
 * unconditionally.
 */
void plat_sleep_idle(plat_idle_pred_t still_idle);

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

#endif /* PLATFORM_IF_H */
