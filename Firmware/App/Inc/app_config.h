/**
 * @file    app_config.h
 * @brief   Level 2 (logic) — compile-time tuning of the attendance application.
 *
 * Every value here is a *policy* decision, not a hardware fact. Hardware facts
 * (pins, peripheral instances, clock trees, reader timings) live in
 * Bsp/Inc/bsp_board.h.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ------------------------------------------------------------------------ */
/* Timing policy                                                            */
/* ------------------------------------------------------------------------ */

/** Inactivity window after which the unit flushes and goes to Standby. */
#define APP_INACTIVITY_MS               (3u * 60u * 1000u)

/** A card presenting the same ID again inside this window is a duplicate. */
#define APP_DEDUP_WINDOW_S              (10u)

/**
 * A card already recorded in the current lecture is not recorded again, however
 * long ago, so a student who taps twice by accident, or comes back to the
 * reader later, is counted once. A lecture lasts until the next one is started
 * from the USB drive; with none started, or a very long one, this is the most
 * time a record is looked back over. It reads the flash log, so it survives
 * the unit going to Standby between taps.
 */
#define APP_SESSION_MAX_AGE_S           (6u * 3600u)

/** Records examined at most per tap by that check (a few hundred microseconds each 100). */
#define APP_SESSION_SCAN_MAX            (2048u)

/** How many distinct recent cards are remembered for duplicate suppression. */
#define APP_DEDUP_SLOTS                 (8u)

/* ------------------------------------------------------------------------ */
/* Card reader policy                                                       */
/* ------------------------------------------------------------------------ */

/** Interval between reader polls. The field is off between polls. */
#define APP_NFC_POLL_MS                 (100u)

/** Field-on time before REQA, so a card can power up (ISO14443-3: 5 ms). */
#define APP_NFC_FIELD_GUARD_MS          (5u)

/** Consecutive empty polls before a held card counts as taken away. Until
 *  then the same card is not reported again, however many polls see it. */
#define APP_NFC_REMOVE_MISSES           (3u)

/** 1: between cards the reader sits in its own wake-up mode and interrupts
 *  on PB1 when something changes the antenna; it polls only from a wake-up
 *  until the field is empty again. 0: poll every APP_NFC_POLL_MS for ever. */
#define APP_NFC_USE_WAKEUP              (1u)

/** Wake-up mode measurement interval: the longest a card waits before the
 *  reader notices it. The chip offers 10-80 ms and 100-800 ms. */
#define APP_NFC_WAKE_PERIOD_MS          (100u)

/** Longest the main loop sleeps with nothing due. Each wake is cheap; this
 *  only bounds how stale the dbg_* globals can get. */
#define APP_SLEEP_MAX_MS                (1000u)

/** Wake-up mode trigger window: counts either side of the reference that do
 *  not wake the reader. It starts at the minimum and widens by one after each
 *  false wake-up beyond the first in a row (the first one corrects the
 *  reference instead), up to the maximum; it narrows by one again after
 *  APP_NFC_WAKE_RELAX_AFTER wake-ups in a row that found a card. */
#define APP_NFC_WAKE_DELTA_MIN          (2u)
#define APP_NFC_WAKE_DELTA_MAX          (10u)
#define APP_NFC_WAKE_RELAX_AFTER        (20u)

/** Retry interval when the reader failed to initialise. */
#define APP_NFC_RETRY_MS                (5000u)

/** Reader supply mode, with hysteresis. The reader's 3.3 V mode is only
 *  allowed up to 3.6 V; below 3.5 V its 5 V mode loses regulator headroom. */
#define APP_NFC_SUPPLY_3V3_BELOW_MV     (3500u)
#define APP_NFC_SUPPLY_5V_ABOVE_MV      (3600u)


/* ------------------------------------------------------------------------ */
/* Button policy                                                            */
/* ------------------------------------------------------------------------ */

#define APP_BTN_DEBOUNCE_MS             (30u)

/** Hold this long for a new lecture: a buzz marks the moment, and letting go
 *  before APP_BTN_OFF_MS starts it. Shorter presses show the battery status. */
#define APP_BTN_LONG_MS                 (2000u)

/** Hold this long to power off instead; no lecture is started. */
#define APP_BTN_OFF_MS                  (5000u)

/** Two taps whose releases are this close are a double press: with the cable
 *  in and the drive ejected, it brings the drive back. */
#define APP_BTN_DOUBLE_MS               (600u)

/** Give up waiting for the button to be released before Standby. */
#define APP_BTN_RELEASE_TIMEOUT_MS      (10000u)

/* ------------------------------------------------------------------------ */
/* Feedback and indicator policy (ms)                                       */
/* ------------------------------------------------------------------------ */

#define APP_FB_ACCEPT_VIB_MS            (90u)
#define APP_FB_ACCEPT_LED_MS            (250u)
#define APP_FB_DUPLICATE_PULSE_MS       (60u)
#define APP_FB_DUPLICATE_GAP_MS         (90u)
#define APP_FB_LOWBATT_BLINK_MS         (150u)
#define APP_FB_LOWBATT_BLINKS           (5u)
#define APP_FB_ERROR_BLINK_MS           (100u)
#define APP_FB_ERROR_BLINKS             (4u)
#define APP_FB_STATUS_BLINK_MS          (150u)
#define APP_FB_POWER_ON_MS              (300u)
#define APP_FB_POWER_ON_VIB_MS          (120u)
#define APP_FB_POWER_OFF_MS             (700u)
#define APP_FB_POWER_OFF_VIB_MS         (250u)
#define APP_FB_HOLD_VIB_MS              (60u)
#define APP_FB_LECTURE_PULSE_MS         (80u)
#define APP_FB_LECTURE_GAP_MS           (120u)

/** Idle heartbeat: one short flash per period, green, or red on a low cell. */
#define APP_IND_IDLE_PERIOD_MS          (4000u)
#define APP_IND_IDLE_ON_MS              (30u)

/** USB session: green flash once a second. */
#define APP_IND_USB_PERIOD_MS           (1000u)
#define APP_IND_USB_ON_MS               (100u)

/* ------------------------------------------------------------------------ */
/* Buffering policy                                                         */
/* ------------------------------------------------------------------------ */

/** Records held in RAM before they are pushed to flash. */
#define APP_RAM_RECORDS                 (128u)

/** Flush threshold, in percent of APP_RAM_RECORDS (flow chart: 80 %). */
#define APP_RAM_FLUSH_PERCENT           (80u)

/** Also flush once no card has been recorded for this long, so a battery
 *  pulled out of a running unit loses seconds of scans rather than a lesson's
 *  worth. Appending is a few double-word programs; no erase is involved. */
#define APP_FLUSH_IDLE_MS               (5000u)

/* ------------------------------------------------------------------------ */
/* Battery policy                                                           */
/* ------------------------------------------------------------------------ */

/** Below this the unit refuses to start / shuts down (single Li-ion, mV). */
#define APP_BATT_CUTOFF_MV              (3300u)

/** Below this the unit still runs but shows a red heartbeat. */
#define APP_BATT_WARN_MV                (3500u)

/** Shut down on a flat cell. 0 only measures and reports. */
#define APP_ENABLE_BATTERY_PROTECTION   (1u)

/** Battery sampling interval while running. */
#define APP_BATT_SAMPLE_MS              (10000u)

/** Consecutive critical samples before a shutdown, so one sample taken during
 *  a load spike cannot switch the unit off. */
#define APP_BATT_CRITICAL_SAMPLES       (3u)

/** Time after reset before a reading can be trusted. C3 (100 nF) across the
 *  divider charges through R7 || R8 (1.7 M), a 171 ms time constant, and
 *  starts empty when a battery is first connected; after 7 time constants
 *  it is within 0.1 %, about 4 mV at the battery. A critical reading taken
 *  before this is checked again once it has passed. */
#define APP_BATT_SETTLE_MS              (1200u)

/**
 * Resistor divider on the battery sense node: Vadc = Vbat * LOW/(LOW+HIGH).
 *
 * R7 4.7 M (BAT+ to the node) over R8 2.7 M (node to GND), C3 100 nF across
 * R8; the node feeds PA7 (ADC1_IN12) and PB7 (PVD_IN). High impedance because
 * the divider is permanently connected: the PVD comparator watches the same
 * node. C3 supplies the charge for the ADC's sampling capacitor, which the
 * 1.7 M source could not, and averages out load transients.
 */
#define APP_BATT_DIV_HIGH_KOHM          (4700u)
#define APP_BATT_DIV_LOW_KOHM           (2700u)

/* ------------------------------------------------------------------------ */
/* USB policy                                                               */
/* ------------------------------------------------------------------------ */

#define APP_VBUS_DEBOUNCE_MS            (50u)

/** VBUS without enumeration for this long is a charger, not a host: USB is
 *  stopped again and the unit keeps scanning while it charges. */
#define APP_USB_ENUM_TIMEOUT_MS         (5000u)

/** After the host ejects the drive, wait this long before dropping off the
 *  bus, so the host finishes its side (udisks powers the port off) first.
 *  Then SETTINGS.CSV is applied and scanning resumes on USB power. */
#define APP_USB_EJECT_GRACE_MS          (1000u)

/* ------------------------------------------------------------------------ */
/* Log / CSV policy                                                         */
/* ------------------------------------------------------------------------ */

/** Epoch used for stored timestamps: seconds since 2000-01-01T00:00:00Z. */
#define APP_EPOCH_YEAR                  (2000)

#endif /* APP_CONFIG_H */
