/**
 * @file    app_fsm.c
 * @brief   Level 2 (logic) — application state machine.
 *
 * Structure
 * ---------
 * app_task() is one turn of a pump: take an event, act on it, and when the
 * queue empties go to sleep. main() calls it in a loop. Nothing here polls and
 * nothing here blocks, which is what makes the duty cycle so low: outside of a
 * 100 ms read the part is in Stop 2, and even the vibration patterns are
 * stepped from a low-power timer.
 *
 * The one deliberate exception is the read phase, which uses Sleep instead of
 * Stop 2 because the capture timer has to keep running.
 */
#include "app_fsm.h"
#include "app_config.h"
#include "platform_if.h"
#include "em4100.h"
#include "student_db.h"
#include "record_buffer.h"
#include "log_store.h"
#include "dedup.h"
#include "feedback.h"
#include "battery.h"
#include "timeutil.h"
#include "usb_storage.h"

/* ------------------------------------------------------------------------ */
/* Context                                                                  */
/* ------------------------------------------------------------------------ */

static struct {
    app_state_t state;

    record_buffer_t rb;
    log_store_t     log;
    student_db_t    db;
    dedup_t         dedup;
    feedback_t      fb;
    app_stats_t     stats;

    /* Capture working set. Static rather than on the stack: together these
     * are about 3.5 kB, which is more than the interrupt stack should carry. */
    uint32_t    edges[EM4100_MAX_EDGES];
    em4100_ws_t ws;

    uint16_t consecutive_false_wakes;
    bool     shutdown_after_feedback;
    bool     battery_low;
} g;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static bool queue_still_empty(void);

static app_epoch_t now_epoch(void)
{
    app_datetime_t dt;

    plat_rtc_get(&dt);
    return time_to_epoch(&dt);
}

/**
 * Return to the idle state, re-arming the card detector.
 *
 * Touch is re-armed here and nowhere else on the scan path, because here is
 * the only point at which both the carrier and the vibration motor are
 * guaranteed to be off. Arming it any earlier would let the motor's own
 * noise couple into the pad and trigger an immediate false wake.
 */
static void go_idle(void)
{
    plat_touch_irq_enable(true);
    g.state = ST_IDLE;
}

/** Start a feedback pattern and arm the timer that steps it. */
static void begin_feedback(fb_pattern_t pattern)
{
    uint16_t ms = fb_start(&g.fb, pattern);

    if (ms == 0u) {
        go_idle();
        return;
    }
    g.state = ST_FEEDBACK;
    plat_timer_start(ms);
}

/** Push the RAM buffer into flash, counting a failure if anything is left. */
static void flush_to_flash(void)
{
    uint16_t pending = rb_count(&g.rb);

    if (pending == 0u) {
        return;
    }
    if (log_flush(&g.log, &g.rb) < pending) {
        g.stats.flush_failures++;
    }
}

/**
 * Everything the flow chart's "3-min inactivity" and low-battery branches have
 * in common before Standby: get the data safe, then hand off to Level 1 which
 * powers the peripherals down and arms the wake pin.
 */
static void shutdown(void) __attribute__((noreturn));
static void shutdown(void)
{
    g.state = ST_SHUTDOWN;

    plat_inactivity_stop();
    plat_timer_stop();
    plat_touch_irq_enable(false);

    flush_to_flash();
    log_seal(&g.log);

    plat_rf_carrier(false);
    plat_rf_power(false);
    plat_touch_power(false);
    plat_out_write(0u);

    plat_sleep_deep();
}

/* ------------------------------------------------------------------------ */
/* Card read                                                                */
/* ------------------------------------------------------------------------ */

/**
 * Flow chart: "Wake MCU, restore clocks / Restart 3-min timer / Power RF
 * reader / Capture card".
 *
 * Touch is disarmed first: the pad sits next to the antenna and the 125 kHz
 * field would otherwise retrigger it continuously for the whole read.
 */
static void start_read(void)
{
    plat_inactivity_restart(APP_INACTIVITY_MS);
    plat_touch_irq_enable(false);

    plat_rf_power(true);
    plat_rf_carrier(true);
    plat_rf_capture_start(g.edges, EM4100_MAX_EDGES);

    g.state = ST_READING;
    plat_timer_start(APP_CARD_READ_TIMEOUT_MS);
}

/** Tear the RF front end down. Ordered so the carrier stops before the rail. */
static void stop_read(void)
{
    plat_timer_stop();
    plat_rf_carrier(false);
    plat_rf_power(false);
}

/** Flow chart: "No card (false wake) / Carrier off, count false wakes". */
static void handle_no_card(void)
{
    g.stats.false_wakes++;
    g.consecutive_false_wakes++;

    /* A pad that keeps firing with nothing near it has drifted. Ask the touch
     * IC to re-run its self-calibration rather than let it burn battery
     * waking the MCU a few hundred times an hour. */
    if (g.consecutive_false_wakes >= APP_FALSE_WAKE_RECAL_LIMIT) {
        g.consecutive_false_wakes = 0u;
        plat_touch_recalibrate();
    }

    go_idle();
}

/**
 * Flow chart: the decision chain from "Valid ID?" down to "RAM buffer >= 80 %".
 */
static void handle_tag(const app_tag_t *tag)
{
    app_epoch_t now = now_epoch();

    g.consecutive_false_wakes = 0u;

    /* "Same ID read within last 10 s?" */
    if (dedup_check_and_mark(&g.dedup, tag->unique_id, now)) {
        g.stats.scans_duplicate++;
        begin_feedback(FB_DUPLICATE);
        return;
    }

    /* "ID in student list?" */
    if (!sdb_contains(&g.db, tag->unique_id)) {
        g.stats.scans_unknown++;
        begin_feedback(FB_UNKNOWN);
        return;
    }

    /* "Create record / Store data in RAM buffer" */
    app_record_t rec;
    rec.student_id = tag->unique_id;
    rec.stamp = now;

    if (rb_push(&g.rb, &rec)) {
        g.stats.scans_accepted++;
    } else {
        g.stats.records_dropped++;
    }

    /* Feedback first, flush second. The user gets their confirmation inside a
     * few milliseconds instead of waiting out a page erase, and the erase then
     * overlaps the tail of the vibration pattern. */
    begin_feedback(g.battery_low ? FB_LOW_BATTERY : FB_ACCEPTED);

    /* "RAM buffer >= 80 % full?" */
    if (rb_needs_flush(&g.rb)) {
        flush_to_flash();
    }
}

/** Capture finished, either full or timed out. Decode and act. */
static void finish_read(void)
{
    uint16_t n = plat_rf_capture_stop();

    stop_read();

    app_tag_t tag;
    em4100_status_t st = em4100_decode(g.edges, n, plat_rf_capture_hz(),
                                       &g.ws, &tag);

    if (st == EM4100_OK) {
        handle_tag(&tag);
    } else {
        handle_no_card();
    }
}

/* ------------------------------------------------------------------------ */
/* USB                                                                      */
/* ------------------------------------------------------------------------ */

/** Flow chart: "Wake MCU, enable USB clock / Pause timer / Flush / Enumerate". */
static void usb_attach(void)
{
    plat_inactivity_stop();
    plat_touch_irq_enable(false);
    fb_cancel(&g.fb);
    plat_timer_stop();

    /* The exported file is a snapshot, so everything in RAM has to be in
     * flash before the size is latched. */
    flush_to_flash();
    log_seal(&g.log);

    app_datetime_t dt;
    plat_rtc_get(&dt);
    usbs_begin(&g.log, sdb_device_id(&g.db), &dt);

    plat_usb_start();
    g.state = ST_USB;
}

/** Flow chart: "Power Down / De initialize USB / Switch off Clock". */
static void usb_detach(void)
{
    plat_usb_stop();
    shutdown();
}

/* ------------------------------------------------------------------------ */
/* Start-up                                                                 */
/* ------------------------------------------------------------------------ */

/** Flow chart: "Battery OK?" — read once at boot to catch a flat cell early. */
static bool battery_startup_ok(void)
{
    app_adc_sample_t sample;

    if (!plat_adc_sample(&sample)) {
        return true;   /* unusable reading is not evidence of a flat battery */
    }

    switch (batt_classify(batt_millivolts(&sample))) {
    case BATT_CRITICAL:
        return false;
    case BATT_WARN:
        g.battery_low = true;
        return true;
    case BATT_OK:
    default:
        g.battery_low = false;
        return true;
    }
}

void app_init(void)
{
    app_event_init();

    g.state = ST_IDLE;
    g.consecutive_false_wakes = 0u;
    g.shutdown_after_feedback = false;
    g.battery_low = false;

    rb_init(&g.rb);
    dedup_init(&g.dedup);
    fb_init(&g.fb);

    uint8_t *p = (uint8_t *)&g.stats;
    uint32_t i;
    for (i = 0u; i < sizeof(g.stats); i++) {
        p[i] = 0u;
    }

    /* "Load student list & config from flash" */
    if (sdb_load(&g.db) && !sdb_verify(&g.db)) {
        /* A corrupt list would reject every genuine card. Better to refuse to
         * use it and let every tag read as unknown, which is visible, than to
         * accept a list that may have silently lost entries. */
        g.db.loaded = false;
    }

    log_init(&g.log);

    if (!battery_startup_ok()) {
        /* "Low-battery warning (red LED blinks)" then straight to Standby.
         * This runs before main()'s app_task() pump exists, so it pumps its own
         * events: sleeping between steps keeps even the warning low power. */
        uint16_t ms = fb_start(&g.fb, FB_LOW_BATTERY);

        while (ms > 0u) {
            plat_timer_start(ms);

            for (;;) {
                plat_sleep_light(queue_still_empty);
                if (app_event_get() == APP_EVT_TIMER) {
                    break;
                }
            }
            ms = fb_advance(&g.fb);
        }
        shutdown();
    }

    plat_touch_power(true);
    plat_touch_recalibrate();
    plat_touch_irq_enable(true);

    plat_inactivity_restart(APP_INACTIVITY_MS);

    /* Plugged into a host at power-up: go straight to the USB branch. */
    if (plat_usb_vbus_present()) {
        usb_attach();
    }
}

/* ------------------------------------------------------------------------ */
/* Event dispatch                                                           */
/* ------------------------------------------------------------------------ */

void app_dispatch(app_event_t evt)
{
    switch (evt) {

    case APP_EVT_TOUCH:
        /* Ignored in every state but idle: mid-read the pad is disarmed
         * anyway, and during a USB session the reader stays off. */
        if (g.state == ST_IDLE) {
            start_read();
        } else if (g.state == ST_FEEDBACK) {
            /* Card still present at the end of a pattern; restart the
             * inactivity window but do not interrupt the feedback. */
            plat_inactivity_restart(APP_INACTIVITY_MS);
        }
        break;

    case APP_EVT_CAPTURE_FULL:
        if (g.state == ST_READING) {
            finish_read();
        }
        break;

    case APP_EVT_TIMER:
        if (g.state == ST_READING) {
            finish_read();               /* the 100 ms timeout */
        } else if (g.state == ST_FEEDBACK) {
            uint16_t ms = fb_advance(&g.fb);
            if (ms > 0u) {
                plat_timer_start(ms);
            } else if (g.shutdown_after_feedback) {
                shutdown();
            } else {
                go_idle();
            }
        }
        break;

    case APP_EVT_INACTIVITY:
        /* Never while the host is attached; the timer is stopped then, but a
         * late interrupt could still be queued. */
        if (g.state != ST_USB) {
            shutdown();
        }
        break;

    case APP_EVT_USB_ATTACH:
        if (g.state != ST_USB) {
            usb_attach();
        }
        break;

    case APP_EVT_USB_DETACH:
        if (g.state == ST_USB) {
            usb_detach();
        }
        break;

    case APP_EVT_USB_ACTIVITY:
        /* Host is reading the volume. Nothing to do beyond staying awake,
         * which we already are. */
        break;

    case APP_EVT_LOW_BATTERY:
        /* Flow chart: flush immediately, warn, then Standby. The flush comes
         * first because the PVD trips well above brown-out but there is no
         * guarantee of how much longer the cell will hold up under a page
         * erase. */
        g.battery_low = true;
        plat_touch_irq_enable(false);
        flush_to_flash();
        log_seal(&g.log);
        g.shutdown_after_feedback = true;
        begin_feedback(FB_LOW_BATTERY);
        break;

    case APP_EVT_BUTTON:
        /* Power button pressed while running: an explicit shutdown request. */
        shutdown();
        break;

    case APP_EVT_NONE:
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Main loop                                                                */
/* ------------------------------------------------------------------------ */

/**
 * Handed to the platform so it can re-check, with interrupts masked, that
 * sleeping is still the right thing to do.
 */
static bool queue_still_empty(void)
{
    return !app_event_pending();
}

/** Pick the deepest sleep the current state allows. */
static void idle_sleep(void)
{
    if (g.state == ST_READING) {
        /* Capture timer and DMA must keep their clock. */
        plat_sleep_idle(queue_still_empty);
    } else if (g.state == ST_USB) {
        /* The USB peripheral needs its 48 MHz clock; Stop 2 would drop the
         * bus and the host would see the device disappear mid-copy. */
        plat_sleep_idle(queue_still_empty);
    } else {
        plat_sleep_light(queue_still_empty);
    }
}

void app_task(void)
{
    app_event_t evt = app_event_get();

    if (evt == APP_EVT_NONE) {
        idle_sleep();
        return;
    }
    app_dispatch(evt);
}

app_state_t app_state(void)
{
    return g.state;
}

const app_stats_t *app_get_stats(void)
{
    return &g.stats;
}
