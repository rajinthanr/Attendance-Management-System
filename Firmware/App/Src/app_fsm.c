/**
 * @file    app_fsm.c
 * @brief   Level 2 (logic) — application state machine, polling mode.
 *
 * Structure
 * ---------
 * app_task() is one pass of a super-loop, run about once a millisecond:
 *
 *   sample button and VBUS     debounced here, posted as events
 *   step the feedback pattern  LEDs and motor, timed against the uptime
 *   run the current state      poll the reader, sample the battery, flush
 *   handle queued events       button, USB, inactivity, low battery
 *   drive the idle indicator   only while no pattern is playing
 *   publish dbg_* globals      for Live Expressions
 *   sleep until next SysTick
 *
 * Nothing blocks. The reader's 5 ms field guard is a timestamp, not a delay,
 * so the button and USB stay responsive while a poll is in progress.
 *
 * Moving to interrupts later means posting the same events from ISRs and
 * replacing the reader poll with the ST25R3916 wake-up interrupt; the
 * handlers below stay as they are.
 */
#include <string.h>

#include "app_config.h"
#include "app_debug.h"
#include "app_fsm.h"
#include "battery.h"
#include "button.h"
#include "card_reader.h"
#include "dedup.h"
#include "feedback.h"
#include "log_store.h"
#include "platform_if.h"
#include "record_buffer.h"
#include "student_db.h"
#include "timeutil.h"
#include "usb_storage.h"

/** Forces the next indicator write after a pattern has driven the outputs. */
#define OUTPUTS_UNKNOWN 0xFFFFFFFFu

/* ------------------------------------------------------------------------ */
/* Context                                                                  */
/* ------------------------------------------------------------------------ */

static struct {
  app_state_t state;

  record_buffer_t rb;
  log_store_t log;
  student_db_t db;
  dedup_t dedup;
  feedback_t fb;
  button_t btn;
  card_reader_t reader;
  app_stats_t stats;

  uint32_t now;         /**< plat_uptime_ms(), sampled once per pass. */
  uint32_t fb_deadline; /**< When the current pattern step ends. */
  uint32_t outputs;     /**< Last indicator mask written. */
  uint32_t last_activity;
  uint32_t last_record;
  uint32_t next_battery;
  uint32_t next_nfc_retry;
  uint32_t next_clock;
  uint32_t shutdown_since;
  uint32_t vbus_changed;
  uint32_t usb_since;

  batt_state_t batt;
  uint8_t batt_critical;
  bool nfc_ready;
  bool nfc_3v3;
  bool vbus_raw;
  bool vbus;         /**< Debounced. */
  bool usb_host;     /**< A host enumerated this session. */
  bool charger_only; /**< VBUS without a host; keep scanning. */
} g;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/** True once @p deadline has been reached. Wrap safe. */
static bool due(uint32_t deadline) { return (int32_t)(g.now - deadline) >= 0; }

/** True once @p ms have passed since @p start. Wrap safe. */
static bool elapsed(uint32_t start, uint32_t ms) {
  return (uint32_t)(g.now - start) >= ms;
}

static app_epoch_t now_epoch(void) {
  app_datetime_t dt;

  plat_rtc_get(&dt);
  return time_to_epoch(&dt);
}

static void touch_activity(void) { g.last_activity = g.now; }

static bool queue_still_empty(void) { return !app_event_pending(); }

/* ------------------------------------------------------------------------ */
/* Feedback and indicator                                                   */
/* ------------------------------------------------------------------------ */

static void begin_feedback(fb_pattern_t pattern) {
  uint16_t ms = fb_start(&g.fb, pattern);

  g.fb_deadline = g.now + ms;
  g.outputs = OUTPUTS_UNKNOWN;
}

static void step_feedback(void) {
  if (fb_is_active(&g.fb) && due(g.fb_deadline)) {
    uint16_t ms = fb_advance(&g.fb);
    g.fb_deadline = g.now + ms;
  }
}

/** What the LEDs show when no pattern is playing. */
static uint32_t indicator_mask(void) {
  switch (g.state) {
  case ST_USB:
    return ((g.now % APP_IND_USB_PERIOD_MS) < APP_IND_USB_ON_MS)
               ? PLAT_OUT_LED_GREEN
               : 0u;

  case ST_IDLE:
    if ((g.now % APP_IND_IDLE_PERIOD_MS) < APP_IND_IDLE_ON_MS) {
      /* Red heartbeat: low cell, or a reader that will not start. */
      return (g.batt != BATT_OK || !g.nfc_ready) ? PLAT_OUT_LED_RED
                                                 : PLAT_OUT_LED_GREEN;
    }
    return 0u;

  case ST_SHUTDOWN:
  default:
    return 0u;
  }
}

static void drive_indicator(void) {
  if (fb_is_active(&g.fb)) {
    return;
  }

  uint32_t mask = indicator_mask();
  if (mask != g.outputs) {
    plat_out_write(mask);
    g.outputs = mask;
  }
}

/* ------------------------------------------------------------------------ */
/* Storage                                                                  */
/* ------------------------------------------------------------------------ */

/** Push the RAM buffer into flash, counting a failure if anything is left. */
static void flush_to_flash(void) {
  uint16_t pending = rb_count(&g.rb);

  if (pending == 0u) {
    return;
  }
  if (log_flush(&g.log, &g.rb) < pending) {
    g.stats.flush_failures++;
  }
}

/* ------------------------------------------------------------------------ */
/* Reader and battery                                                       */
/* ------------------------------------------------------------------------ */

/** Reader supply mode for @p mv, with hysteresis around the 3.6 V limit. */
static bool nfc_supply_3v3_for(uint32_t mv, bool current) {
  if (mv == 0u) {
    return current;
  }
  if (mv < APP_NFC_SUPPLY_3V3_BELOW_MV) {
    return true;
  }
  if (mv > APP_NFC_SUPPLY_5V_ABOVE_MV) {
    return false;
  }
  return current;
}

static void start_reader(void) {
  uint8_t chip_id = 0u;

  g.nfc_ready = plat_nfc_init(g.nfc_3v3, &chip_id);
  dbg_nfc_chip_id = chip_id;
  dbg_nfc_ready = g.nfc_ready;

  if (g.nfc_ready) {
    uint8_t amplitude = 0u;
    if (plat_nfc_measure_amplitude(&amplitude)) {
      dbg_nfc_amplitude = amplitude;
    }
  } else {
    g.stats.nfc_init_failures++;
    g.next_nfc_retry = g.now + APP_NFC_RETRY_MS;
  }
}

/** Flow chart: "Battery OK?", repeated every APP_BATT_SAMPLE_MS. */
static void sample_battery(void) {
  app_adc_sample_t sample;

  batt_reading_t reading;

  g.next_battery = g.now + APP_BATT_SAMPLE_MS;
  dbg_battery_samples++;

  if (!plat_adc_sample(&sample)) {
    dbg_battery_error = 1u; /* see dbg_adc_error for the step that failed */
    return;
  }

  /* The raw material is published on every attempt, accepted or not. */
  batt_evaluate(&sample, &reading);
  dbg_battery_counts = sample.vbat_counts;
  dbg_vrefint_counts = sample.vrefint_counts;
  dbg_vrefint_cal = sample.vrefint_cal;
  dbg_vdda_mv = reading.vdda_mv;
  dbg_battery_raw_mv = reading.vbat_mv;
  dbg_battery_error = (reading.status == BATT_SAMPLE_OK)
                          ? 0u
                          : (uint8_t)(1u + (uint8_t)reading.status);

  if (reading.status != BATT_SAMPLE_OK) {
    return; /* an unusable sample is not evidence of a flat battery */
  }
  uint32_t mv = reading.vbat_mv;

  g.batt = batt_classify(mv);
  dbg_battery_mv = mv;
  dbg_battery_state = (uint8_t)g.batt;

  bool want_3v3 = nfc_supply_3v3_for(mv, g.nfc_3v3);
  if (want_3v3 != g.nfc_3v3) {
    g.nfc_3v3 = want_3v3;
    if (g.nfc_ready && !plat_nfc_set_supply(want_3v3)) {
      g.nfc_ready = false; /* retried by run_idle() */
      g.next_nfc_retry = g.now;
    }
  }
  dbg_nfc_supply_3v3 = g.nfc_3v3;

#if APP_ENABLE_BATTERY_PROTECTION
  /* A cell on charge is not about to brown out. */
  if (g.batt == BATT_CRITICAL && !g.vbus) {
    g.batt_critical++;
    if (g.batt_critical >= APP_BATT_CRITICAL_SAMPLES) {
      g.batt_critical = 0u;
      app_event_post(APP_EVT_LOW_BATTERY);
    }
  } else {
    g.batt_critical = 0u;
  }
#endif
}

/* ------------------------------------------------------------------------ */
/* Card handling                                                            */
/* ------------------------------------------------------------------------ */

/** Flow chart: "ID in student list?" */
static bool student_allowed(uint32_t id) {
  if (!g.db.loaded) {
    return APP_ACCEPT_ALL_WHEN_NO_LIST != 0u;
  }
  return sdb_contains(&g.db, id);
}

static void publish_card(const iso14443a_card_t *card, uint32_t id) {
  uint8_t i;

  for (i = 0u; i < sizeof(dbg_card_uid); i++) {
    dbg_card_uid[i] = (i < card->uid_len) ? card->uid[i] : 0u;
  }
  dbg_card_uid_len = card->uid_len;
  dbg_card_atqa[0] = card->atqa[0];
  dbg_card_atqa[1] = card->atqa[1];
  dbg_card_sak = card->sak;
  dbg_card_id = id;
  dbg_card_count++;
}

/** Flow chart: the decision chain from "Valid ID?" to "RAM buffer >= 80 %". */
static void handle_card(const iso14443a_card_t *card) {
  uint32_t id = card_id_from_uid(card->uid, card->uid_len);
  app_epoch_t stamp = now_epoch();
  app_scan_result_t result;

  touch_activity();
  publish_card(card, id);

  if (!student_allowed(id)) {
    g.stats.scans_unknown++;
    result = APP_SCAN_UNKNOWN;
    begin_feedback(FB_UNKNOWN);
  } else if (dedup_check_and_mark(&g.dedup, id, stamp)) {
    /* "Same ID read within last 10 s?" */
    g.stats.scans_duplicate++;
    result = APP_SCAN_DUPLICATE;
    begin_feedback(FB_DUPLICATE);
  } else if (log_remaining(&g.log) <= rb_count(&g.rb)) {
    /* Nowhere left to put it. Say so on every scan, rather than accept a
     * record that can never be exported. */
    g.stats.scans_rejected_full++;
    result = APP_SCAN_STORAGE_FULL;
    begin_feedback(FB_ERROR);
  } else {
    app_record_t rec;
    rec.student_id = id;
    rec.stamp = stamp;

    if (rb_push(&g.rb, &rec)) {
      g.stats.scans_accepted++;
      g.last_record = g.now;
      result = APP_SCAN_ACCEPTED;
      begin_feedback(FB_ACCEPTED);
    } else {
      g.stats.records_dropped++;
      result = APP_SCAN_STORAGE_FULL;
      begin_feedback(FB_ERROR);
    }

    /* Feedback first, flush second: the user's confirmation starts before
     * any flash programming does. */
    if (rb_needs_flush(&g.rb)) {
      flush_to_flash();
    }
  }

  dbg_scan_result = (uint8_t)result;
}

/* ------------------------------------------------------------------------ */
/* Power off                                                                */
/* ------------------------------------------------------------------------ */

/**
 * Everything the "3-min inactivity", low-battery and button branches have in
 * common: stop the reader, get the data safe, then play @p pattern. Standby
 * follows once it has finished and the button is up (run_shutdown()).
 */
static void begin_shutdown(fb_pattern_t pattern) {
  cr_enable(&g.reader, false, g.now);
  if (g.state == ST_USB) {
    plat_usb_stop();
  }
  flush_to_flash();

  g.state = ST_SHUTDOWN;
  g.shutdown_since = g.now;
  begin_feedback(pattern);
}

static void power_off(void) __attribute__((noreturn));
static void power_off(void) {
  flush_to_flash();
  plat_out_write(0u);
  plat_nfc_power_down();
  plat_sleep_deep();
}

static void run_shutdown(void) {
  if (fb_is_active(&g.fb)) {
    return;
  }
  /* Standby wakes on the button. Entering it with the button still held
   * would make the release of this very press look like a new one. */
  if (btn_is_down(&g.btn) &&
      !elapsed(g.shutdown_since, APP_BTN_RELEASE_TIMEOUT_MS)) {
    return;
  }
  power_off();
}

/* ------------------------------------------------------------------------ */
/* USB                                                                      */
/* ------------------------------------------------------------------------ */

/** Flow chart: "Wake MCU, enable USB clock / Pause timer / Flush / Enumerate".
 */
static void usb_attach(void) {
  app_datetime_t dt;

  if (g.state != ST_IDLE || g.charger_only) {
    return;
  }

  cr_enable(&g.reader, false, g.now);
  fb_cancel(&g.fb);
  g.outputs = OUTPUTS_UNKNOWN;

  /* The exported file is a snapshot of flash, so RAM has to be there first. */
  flush_to_flash();

  plat_rtc_get(&dt);
  usbs_begin(&g.log, sdb_device_id(&g.db), &dt);
  plat_usb_start();

  g.state = ST_USB;
  g.usb_since = g.now;
  g.usb_host = false;
}

/** Back to scanning, after a host session or on finding only a charger. */
static void usb_leave(void) {
  plat_usb_stop();
  g.state = ST_IDLE;
  g.outputs = OUTPUTS_UNKNOWN;
  touch_activity();
}

static void run_usb(void) {
  touch_activity();

  if (plat_usb_configured()) {
    g.usb_host = true;
  } else if (!g.usb_host && elapsed(g.usb_since, APP_USB_ENUM_TIMEOUT_MS)) {
    /* Power without a host: a charger. Charge and keep scanning. */
    g.charger_only = true;
    usb_leave();
    return;
  }

  if (due(g.next_battery)) {
    sample_battery();
  }
}

/* ------------------------------------------------------------------------ */
/* Idle: scanning                                                           */
/* ------------------------------------------------------------------------ */

static void run_idle(void) {
  iso14443a_card_t card;

  if (!g.nfc_ready && due(g.next_nfc_retry)) {
    start_reader();
  }

  /* The reader pauses while a pattern plays: the motor is noisy, and a card
   * held through the pattern is remembered, not reported twice. */
  cr_enable(&g.reader, g.nfc_ready && !fb_is_active(&g.fb), g.now);
  if (cr_task(&g.reader, g.now, &card)) {
    handle_card(&card);
  }

  /* Measurements and flash work only between polls, with the field off.
   * The battery also waits out any pattern, so the motor's current is not
   * pulling the cell down while it is measured. */
  if (!cr_busy(&g.reader)) {
    if (due(g.next_battery) && !fb_is_active(&g.fb)) {
      sample_battery();
      if (g.nfc_ready) {
        uint8_t amplitude = 0u;
        if (plat_nfc_measure_amplitude(&amplitude)) {
          dbg_nfc_amplitude = amplitude;
        }
      }
    }
    if (rb_count(&g.rb) > 0u && elapsed(g.last_record, APP_FLUSH_IDLE_MS)) {
      flush_to_flash();
    }
  }

  if (elapsed(g.last_activity, APP_INACTIVITY_MS)) {
    app_event_post(APP_EVT_INACTIVITY);
  }
}

/* ------------------------------------------------------------------------ */
/* Inputs                                                                   */
/* ------------------------------------------------------------------------ */

static void poll_button(void) {
  switch (btn_update(&g.btn, plat_button_pressed(), g.now)) {
  case BTN_SHORT:
    dbg_button_short_count++;
    app_event_post(APP_EVT_BUTTON_SHORT);
    break;
  case BTN_LONG:
    dbg_button_long_count++;
    app_event_post(APP_EVT_BUTTON_LONG);
    break;
  case BTN_NONE:
  default:
    break;
  }
}

static void poll_vbus(void) {
  bool raw = plat_usb_vbus_present();

  if (raw != g.vbus_raw) {
    g.vbus_raw = raw;
    g.vbus_changed = g.now;
  } else if (raw != g.vbus && elapsed(g.vbus_changed, APP_VBUS_DEBOUNCE_MS)) {
    g.vbus = raw;
    app_event_post(raw ? APP_EVT_USB_ATTACH : APP_EVT_USB_DETACH);
  }
}

/* ------------------------------------------------------------------------ */
/* Debug                                                                    */
/* ------------------------------------------------------------------------ */

static void apply_debug_requests(void) {
  if (dbg_set_time_request) {
    app_datetime_t dt = dbg_set_time;

    if (time_is_valid(&dt)) {
      plat_rtc_set(&dt);
      g.next_clock = g.now;
    }
    dbg_set_time_request = false;
  }
}

static void publish_status(void) {
  dbg_state = (uint8_t)g.state;
  dbg_uptime_ms = g.now;
  dbg_vbus = g.vbus;
  dbg_usb_host = g.usb_host;
  dbg_button_down = btn_is_down(&g.btn);

  dbg_records_ram = rb_count(&g.rb);
  dbg_records_flash = log_total(&g.log);
  dbg_records_free = log_remaining(&g.log);

  dbg_nfc_ready = g.nfc_ready;
  dbg_nfc_last_status = (uint8_t)g.reader.last_status;
  dbg_nfc_polls = g.reader.polls;
  dbg_nfc_errors = g.reader.errors;
  dbg_nfc_collisions = g.reader.collisions;

  if (due(g.next_clock)) {
    app_datetime_t dt;

    g.next_clock = g.now + 1000u;
    plat_rtc_get(&dt);
    dbg_now = dt;
  }
}

/* ------------------------------------------------------------------------ */
/* Start-up                                                                 */
/* ------------------------------------------------------------------------ */

void app_init(void) {
  app_datetime_t dt;

  memset(&g, 0, sizeof(g));
  g.now = plat_uptime_ms();
  g.state = ST_IDLE;
  g.batt = BATT_OK;
  g.outputs = OUTPUTS_UNKNOWN;
  g.last_activity = g.now;
  g.last_record = g.now;
  g.next_clock = g.now;

  app_event_init();
  dbg_boot_cause = (uint8_t)plat_boot_cause();

  rb_init(&g.rb);
  dedup_init(&g.dedup);
  fb_init(&g.fb);
  cr_init(&g.reader);
  btn_init(&g.btn, plat_button_pressed(), g.now);

  /* "Load student list & config from flash" */
  if (sdb_load(&g.db) && !sdb_verify(&g.db)) {
    /* A corrupt list could reject genuine cards. Refuse it outright. */
    g.db.loaded = false;
  }
  dbg_students = sdb_count(&g.db);

  log_init(&g.log);

  /* An RTC that has never been set starts from the build time, which on a
   * freshly flashed unit is within minutes of the truth. */
  if (!plat_rtc_is_valid() && time_from_build(__DATE__, __TIME__, &dt)) {
    plat_rtc_set(&dt);
  }

  /* "Battery OK?" — also picks the reader's supply mode, starting from the
   * 5 V mode, which is the safe side of the 3.6 V boundary. */
  sample_battery();

#if APP_ENABLE_BATTERY_PROTECTION
  if (g.batt == BATT_CRITICAL && !plat_usb_vbus_present()) {
    if (elapsed(0u, APP_BATT_SETTLE_MS)) {
      /* "Low-battery warning (red LED blinks)", then straight back off. */
      begin_shutdown(FB_LOW_BATTERY);
      return;
    }
    /* Too soon after a battery was connected: C3 may still be charging.
     * One more critical reading once it has settled shuts the unit down. */
    g.batt_critical = (uint8_t)(APP_BATT_CRITICAL_SAMPLES - 1u);
    g.next_battery = APP_BATT_SETTLE_MS;
  }
#endif

  start_reader();
  begin_feedback(g.nfc_ready ? FB_POWER_ON : FB_ERROR);
}

/* ------------------------------------------------------------------------ */
/* Event dispatch                                                           */
/* ------------------------------------------------------------------------ */

void app_dispatch(app_event_t evt) {
  switch (evt) {
  case APP_EVT_BUTTON_SHORT:
    /* A tap shows the battery: two green blinks, or the red low pattern. */
    if (g.state != ST_SHUTDOWN) {
      touch_activity();
      begin_feedback(g.batt == BATT_OK ? FB_STATUS_OK : FB_LOW_BATTERY);
    }
    break;

  case APP_EVT_BUTTON_LONG:
    if (g.state != ST_SHUTDOWN) {
      begin_shutdown(FB_POWER_OFF);
    }
    break;

  case APP_EVT_INACTIVITY:
    if (g.state == ST_IDLE) {
      begin_shutdown(FB_POWER_OFF);
    }
    break;

  case APP_EVT_USB_ATTACH:
    usb_attach();
    break;

  case APP_EVT_USB_DETACH:
    g.charger_only = false;
    if (g.state == ST_USB) {
      usb_leave();
    }
    break;

  case APP_EVT_USB_ACTIVITY:
    /* The host is reading the volume; run_usb() keeps the unit awake. */
    g.usb_host = true;
    break;

  case APP_EVT_LOW_BATTERY:
    /* Flow chart: flush, warn, then Standby. begin_shutdown() flushes
     * before the pattern starts. */
    if (g.state == ST_IDLE) {
      begin_shutdown(FB_LOW_BATTERY);
    }
    break;

  case APP_EVT_NONE:
  default:
    break;
  }
}

/* ------------------------------------------------------------------------ */
/* Main loop                                                                */
/* ------------------------------------------------------------------------ */

void app_task(void) {
  app_event_t evt;

  g.now = plat_uptime_ms();

  poll_button();
  poll_vbus();
  step_feedback();

  switch (g.state) {
  case ST_IDLE:
    run_idle();
    break;
  case ST_USB:
    run_usb();
    break;
  case ST_SHUTDOWN:
  default:
    run_shutdown();
    break;
  }

  while ((evt = app_event_get()) != APP_EVT_NONE) {
    app_dispatch(evt);
  }

  apply_debug_requests();
  drive_indicator();
  publish_status();

  plat_sleep_idle(queue_still_empty);
}

app_state_t app_state(void) { return g.state; }

const app_stats_t *app_get_stats(void) { return &g.stats; }
