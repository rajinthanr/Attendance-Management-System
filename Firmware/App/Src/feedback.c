/**
 * @file    feedback.c
 * @brief   Level 2 (logic) — feedback pattern tables and stepping.
 *
 * The flow chart calls for vibration only while the reader field is off, which
 * the FSM guarantees by pausing the reader for the length of a pattern; this
 * module is purely "what to drive, and for how long".
 */
#include "feedback.h"
#include "app_config.h"
#include "platform_if.h"

#define GREEN  PLAT_OUT_LED_GREEN
#define RED    PLAT_OUT_LED_RED
#define VIB    PLAT_OUT_VIBRATION

/* Green plus a short buzz, then the LED alone for the rest of its dwell so the
 * user still sees the confirmation after the motor has stopped. */
static const fb_step_t k_accepted[] = {
    { GREEN | VIB, APP_FB_ACCEPT_VIB_MS },
    { GREEN,       APP_FB_ACCEPT_LED_MS - APP_FB_ACCEPT_VIB_MS },
    { 0u,          0u }
};

static const fb_step_t k_duplicate[] = {
    { GREEN | VIB, APP_FB_DUPLICATE_PULSE_MS },
    { 0u,          APP_FB_DUPLICATE_GAP_MS   },
    { GREEN | VIB, APP_FB_DUPLICATE_PULSE_MS },
    { 0u,          0u }
};

/* A card that is not on the registered list: red and one long buzz. It is
 * still recorded, so the PC can offer to register it. */
static const fb_step_t k_unknown[] = {
    { RED | VIB, APP_FB_UNKNOWN_VIB_MS },
    { 0u,        0u }
};

/* Settings import finished. Two pulses and a long green hold for success;
 * three red pulses for a refusal, so the two cannot be mistaken for each other
 * across a room. */
static const fb_step_t k_saved[] = {
    { GREEN | VIB, 90u  },
    { GREEN,       90u  },
    { GREEN | VIB, 90u  },
    { GREEN,       600u },
    { 0u,          0u   }
};

static const fb_step_t k_rejected[] = {
    { RED | VIB, 150u },
    { RED,       100u },
    { RED | VIB, 150u },
    { RED,       100u },
    { RED | VIB, 150u },
    { RED,       400u },
    { 0u,        0u   }
};

static const fb_step_t k_power_on[] = {
    { GREEN | VIB, APP_FB_POWER_ON_VIB_MS },
    { GREEN,       APP_FB_POWER_ON_MS - APP_FB_POWER_ON_VIB_MS },
    { 0u,          0u }
};

static const fb_step_t k_power_off[] = {
    { RED | VIB, APP_FB_POWER_OFF_VIB_MS },
    { RED,       APP_FB_POWER_OFF_MS - APP_FB_POWER_OFF_VIB_MS },
    { 0u,        0u }
};

/* Felt rather than seen, while the button is still held: let go now for a
 * new lecture, keep holding to switch off. */
static const fb_step_t k_hold[] = {
    { VIB, APP_FB_HOLD_VIB_MS },
    { 0u,  0u }
};

/* A new lecture: three quick pulses, unlike anything a card tap produces, so
 * the lecturer can tell without looking that the press took. */
static const fb_step_t k_lecture[] = {
    { GREEN | VIB, APP_FB_LECTURE_PULSE_MS },
    { GREEN,       APP_FB_LECTURE_GAP_MS   },
    { GREEN | VIB, APP_FB_LECTURE_PULSE_MS },
    { GREEN,       APP_FB_LECTURE_GAP_MS   },
    { GREEN | VIB, APP_FB_LECTURE_PULSE_MS },
    { GREEN,       400u },
    { 0u,          0u   }
};

/* Blink tables are built at run time so the counts stay policy knobs rather
 * than hand-unrolled tables. Each holds on/off pairs plus a terminator. */
static fb_step_t s_lowbatt[(APP_FB_LOWBATT_BLINKS * 2u) + 1u];
static fb_step_t s_error[(APP_FB_ERROR_BLINKS * 2u) + 1u];
static fb_step_t s_status[(2u * 2u) + 1u];

static void build_blinks(fb_step_t *table, uint8_t blinks, uint32_t outputs,
                         uint16_t ms)
{
    uint8_t i;

    for (i = 0u; i < blinks; i++) {
        table[i * 2u].outputs = outputs;
        table[i * 2u].ms = ms;
        table[(i * 2u) + 1u].outputs = 0u;
        table[(i * 2u) + 1u].ms = ms;
    }
    table[blinks * 2u].outputs = 0u;
    table[blinks * 2u].ms = 0u;
}

#define USE_TABLE(fb, t) do {                                  \
        (fb)->steps = (t);                                     \
        (fb)->n_steps = (uint8_t)(sizeof(t) / sizeof((t)[0])); \
    } while (0)

void fb_init(feedback_t *fb)
{
    build_blinks(s_lowbatt, APP_FB_LOWBATT_BLINKS, RED, APP_FB_LOWBATT_BLINK_MS);
    build_blinks(s_error, APP_FB_ERROR_BLINKS, RED | VIB, APP_FB_ERROR_BLINK_MS);
    build_blinks(s_status, 2u, GREEN, APP_FB_STATUS_BLINK_MS);
    fb->steps = NULL;
    fb->n_steps = 0u;
    fb->index = 0u;
    fb->active = false;
    plat_out_write(0u);
}

uint16_t fb_start(feedback_t *fb, fb_pattern_t pattern)
{
    switch (pattern) {
    case FB_ACCEPTED:    USE_TABLE(fb, k_accepted);  break;
    case FB_DUPLICATE:   USE_TABLE(fb, k_duplicate); break;
    case FB_UNKNOWN:     USE_TABLE(fb, k_unknown);   break;
    case FB_LOW_BATTERY: USE_TABLE(fb, s_lowbatt);   break;
    case FB_ERROR:       USE_TABLE(fb, s_error);     break;
    case FB_STATUS_OK:   USE_TABLE(fb, s_status);    break;
    case FB_POWER_ON:    USE_TABLE(fb, k_power_on);  break;
    case FB_POWER_OFF:   USE_TABLE(fb, k_power_off); break;
    case FB_SAVED:       USE_TABLE(fb, k_saved);     break;
    case FB_REJECTED:    USE_TABLE(fb, k_rejected);  break;
    case FB_HOLD:        USE_TABLE(fb, k_hold);      break;
    case FB_LECTURE:     USE_TABLE(fb, k_lecture);   break;
    case FB_NONE:
    default:
        fb_cancel(fb);
        return 0u;
    }

    fb->index = 0u;
    fb->active = true;
    plat_out_write(fb->steps[0].outputs);

    uint16_t ms = fb->steps[0].ms;
    if (ms == 0u) {
        fb_cancel(fb);
    }
    return ms;
}

uint16_t fb_advance(feedback_t *fb)
{
    if (!fb->active) {
        return 0u;
    }

    fb->index++;
    if (fb->index >= fb->n_steps) {
        fb_cancel(fb);
        return 0u;
    }

    plat_out_write(fb->steps[fb->index].outputs);

    uint16_t ms = fb->steps[fb->index].ms;
    if (ms == 0u) {
        /* Duration zero is the table's terminator. */
        fb_cancel(fb);
    }
    return ms;
}

bool fb_is_active(const feedback_t *fb)
{
    return fb->active;
}

void fb_cancel(feedback_t *fb)
{
    fb->active = false;
    fb->index = 0u;
    plat_out_write(0u);
}
