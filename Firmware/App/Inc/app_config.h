/**
 * @file    app_config.h
 * @brief   Level 2 (logic) — compile-time tuning of the attendance application.
 *
 * Every value here is a *policy* decision, not a hardware fact. Hardware facts
 * (pins, timer instances, clock trees) live in Bsp/Inc/bsp_board.h.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ------------------------------------------------------------------------ */
/* Timing policy                                                            */
/* ------------------------------------------------------------------------ */

/** Inactivity window after which the unit flushes and goes to Standby. */
#define APP_INACTIVITY_MS               (3u * 60u * 1000u)

/** Maximum time the 125 kHz carrier stays on waiting for a decodable frame. */
#define APP_CARD_READ_TIMEOUT_MS        (100u)

/** A card presenting the same ID again inside this window is a duplicate. */
#define APP_DEDUP_WINDOW_S              (10u)

/** How many distinct recent cards are remembered for duplicate suppression. */
#define APP_DEDUP_SLOTS                 (8u)

/* Feedback pattern durations (ms). */
#define APP_FB_ACCEPT_VIB_MS            (90u)
#define APP_FB_ACCEPT_LED_MS            (250u)
#define APP_FB_DUPLICATE_PULSE_MS       (60u)
#define APP_FB_DUPLICATE_GAP_MS         (90u)
#define APP_FB_REJECT_VIB_MS            (450u)
#define APP_FB_LOWBATT_BLINK_MS         (150u)
#define APP_FB_LOWBATT_BLINKS           (5u)

/* ------------------------------------------------------------------------ */
/* Buffering policy                                                         */
/* ------------------------------------------------------------------------ */

/** Records held in RAM before they are pushed to flash. */
#define APP_RAM_RECORDS                 (128u)

/** Flush threshold, in percent of APP_RAM_RECORDS (flow chart: 80 %). */
#define APP_RAM_FLUSH_PERCENT           (80u)

/* ------------------------------------------------------------------------ */
/* Battery policy                                                           */
/* ------------------------------------------------------------------------ */

/** Below this the unit refuses to start / shuts down (single Li-ion, mV). */
#define APP_BATT_CUTOFF_MV              (3300u)

/** Below this the unit still runs but warns on every scan. */
#define APP_BATT_WARN_MV                (3500u)

/**
 * Resistor divider on the battery sense node: Vadc = Vbat * LOW/(LOW+HIGH).
 *
 * 4.7 M + 4.7 M rather than anything lower because this divider is permanently
 * connected: the PVD comparator watches the same node, and gating it would
 * blind the brown-out detection during exactly the current spikes that cause
 * one. 0.4 uA is the price; a 100 nF cap across the low leg keeps the source
 * impedance low enough for the ADC's 640.5-cycle sampling window.
 */
#define APP_BATT_DIV_HIGH_KOHM          (4700u)
#define APP_BATT_DIV_LOW_KOHM           (4700u)

/* ------------------------------------------------------------------------ */
/* RF / EM4100 policy                                                       */
/* ------------------------------------------------------------------------ */

/* The carrier frequency and the tank settling time are properties of the
 * antenna hardware, not of this application, so they live in
 * Bsp/Inc/bsp_board.h as BSP_RF_CARRIER_HZ and BSP_RF_SETTLE_MS. */

/** Consecutive false wake-ups tolerated before the touch pad is recalibrated. */
#define APP_FALSE_WAKE_RECAL_LIMIT      (20u)

/* ------------------------------------------------------------------------ */
/* Log / CSV policy                                                         */
/* ------------------------------------------------------------------------ */

/** Epoch used for stored timestamps: seconds since 2000-01-01T00:00:00Z. */
#define APP_EPOCH_YEAR                  (2000)

#endif /* APP_CONFIG_H */
