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
 *     capture timestamps stay timer ticks, the RTC returns calendar fields.
 *     Every conversion is Level 2's job.
 *   - Nothing below makes a decision. There is no plat_handle_card(); Level 1
 *     only moves bytes and toggles pins.
 *   - Level 1 never calls into Level 2 except through app_event_post().
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
/* Power modes                                                              */
/* ======================================================================== */

/**
 * Predicate the sleep calls use to decide whether sleeping is still correct.
 * Returns true while the caller has nothing to do.
 */
typedef bool (*plat_idle_pred_t)(void);

/**
 * Core halted, peripherals and clocks left running (STM32 Sleep mode).
 *
 * This is the only mode usable while a card is being captured: the capture
 * timer and its DMA must keep counting, and Stop 2 would gate their clock.
 * Costs more than Stop 2, but only for the 100 ms of a read.
 *
 * @p still_idle is re-tested with interrupts masked, immediately before the
 * core is halted. Without that the caller would race its own interrupts: an
 * event posted between "is my queue empty?" and the sleep instruction would
 * leave the device asleep with work outstanding until some unrelated
 * interrupt happened to wake it. Passing NULL sleeps unconditionally.
 */
void plat_sleep_idle(plat_idle_pred_t still_idle);

/**
 * "Light sleep" in the flow chart: STM32 Stop 2. RAM and register state are
 * retained; wake sources are the touch pad, USB VBUS, PVD and LPTIM1/LPTIM2.
 * Returns once an enabled wake source has fired and clocks are restored.
 *
 * Takes the same predicate, for the same reason, as plat_sleep_idle().
 */
void plat_sleep_light(plat_idle_pred_t still_idle);

/**
 * "Deep sleep" in the flow chart: STM32 Standby. RAM is lost and the only
 * wake source is the power button on the WKUP pin. Does not return; the part
 * resets into main() on wake.
 */
void plat_sleep_deep(void) __attribute__((noreturn));

/** Enter/leave an interrupt-free section. Nestable. */
void plat_critical_enter(void);
void plat_critical_exit(void);

/** Why we booted. Lets the FSM distinguish a Standby wake from a cold start. */
app_boot_cause_t plat_boot_cause(void);

/* ======================================================================== */
/* RF front end (125 kHz reader)                                            */
/* ======================================================================== */

/** Gate the supply to the RF front end (load switch). */
void plat_rf_power(bool on);

/** Start/stop the carrier PWM into the antenna tank. */
void plat_rf_carrier(bool on);

/**
 * Begin capturing both edges of the demodulated tag envelope.
 *
 * @param buf   Caller-owned array that Level 1 fills with raw timer ticks.
 * @param cap   Capacity of @p buf in entries.
 *
 * Capture is DMA-driven and free-running; it stops on its own when @p cap is
 * reached. The tick rate is fixed and reported by plat_rf_capture_hz().
 */
void plat_rf_capture_start(uint32_t *buf, uint16_t cap);

/** Stop capture and return how many edge timestamps landed in the buffer. */
uint16_t plat_rf_capture_stop(void);

/** Capture timebase in Hz, so Level 2 can turn ticks into microseconds. */
uint32_t plat_rf_capture_hz(void);

/* ======================================================================== */
/* Capacitive touch IC (card-presence wake source)                          */
/* ======================================================================== */

/** Gate the supply to the touch IC. */
void plat_touch_power(bool on);

/**
 * Arm/disarm the touch interrupt. Disarmed while the carrier runs: the
 * 125 kHz field couples straight into the pad.
 */
void plat_touch_irq_enable(bool enable);

/** Pulse the touch IC's reset so it re-runs its self-calibration. */
void plat_touch_recalibrate(void);

/* ======================================================================== */
/* Timers                                                                   */
/* ======================================================================== */

/**
 * (Re)start the inactivity timer. Fires APP_EVT_INACTIVITY once after
 * @p ms with no further calls. Must survive Stop 2, so Level 1 implements
 * it on LPTIM1/LSE rather than a TIMx that stops with the core clock.
 */
void plat_inactivity_restart(uint32_t ms);
void plat_inactivity_stop(void);

/**
 * One-shot short delay. Fires APP_EVT_TIMER once after @p ms. Used to step
 * the feedback sequencer and to bound the card read, so it too must run in
 * Stop 2 (LPTIM2/LSE).
 */
void plat_timer_start(uint32_t ms);
void plat_timer_stop(void);

/** Free-running millisecond counter, monotonic across Stop 2. */
uint32_t plat_uptime_ms(void);

/* ======================================================================== */
/* Real-time clock                                                          */
/* ======================================================================== */

void plat_rtc_get(app_datetime_t *out);
void plat_rtc_set(const app_datetime_t *dt);

/** True once the RTC has been set at least once (tracked in a backup register). */
bool plat_rtc_is_valid(void);

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
 * Take one battery + VREFINT sample pair. Blocking, a few hundred
 * microseconds. Returns raw counts only.
 */
bool plat_adc_sample(app_adc_sample_t *out);

/* ======================================================================== */
/* USB device                                                               */
/* ======================================================================== */

bool plat_usb_vbus_present(void);

/** Bring up the USB peripheral and enumerate as a mass-storage device. */
void plat_usb_start(void);

/** Tear USB down and release its clocks. */
void plat_usb_stop(void);

#endif /* PLATFORM_IF_H */
