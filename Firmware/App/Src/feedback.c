/**
 * @file    feedback.c
 * @brief   Level 2 (logic) — feedback pattern tables and stepping.
 *
 * The flow chart calls for vibration only after the carrier is off, which the
 * FSM guarantees by ordering; this module is purely "what to drive, and for
 * how long".
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
    { VIB, APP_FB_DUPLICATE_PULSE_MS },
    { 0u,  APP_FB_DUPLICATE_GAP_MS   },
    { VIB, APP_FB_DUPLICATE_PULSE_MS },
    { 0u,  0u }
};

static const fb_step_t k_unknown[] = {
    { RED | VIB, APP_FB_REJECT_VIB_MS },
    { 0u,        0u }
};

/* Built at run time from APP_FB_LOWBATT_BLINKS so the count stays a policy
 * knob rather than a hand-unrolled table. */
static fb_step_t s_lowbatt[(APP_FB_LOWBATT_BLINKS * 2u) + 1u];

static void build_lowbatt(void)
{
    uint8_t i;

    for (i = 0u; i < APP_FB_LOWBATT_BLINKS; i++) {
        s_lowbatt[i * 2u].outputs     = RED;
        s_lowbatt[i * 2u].ms          = APP_FB_LOWBATT_BLINK_MS;
        s_lowbatt[(i * 2u) + 1u].outputs = 0u;
        s_lowbatt[(i * 2u) + 1u].ms      = APP_FB_LOWBATT_BLINK_MS;
    }
    s_lowbatt[APP_FB_LOWBATT_BLINKS * 2u].outputs = 0u;
    s_lowbatt[APP_FB_LOWBATT_BLINKS * 2u].ms      = 0u;
}

void fb_init(feedback_t *fb)
{
    build_lowbatt();
    fb->steps = NULL;
    fb->n_steps = 0u;
    fb->index = 0u;
    fb->active = false;
    plat_out_write(0u);
}

uint16_t fb_start(feedback_t *fb, fb_pattern_t pattern)
{
    switch (pattern) {
    case FB_ACCEPTED:
        fb->steps = k_accepted;
        fb->n_steps = (uint8_t)(sizeof(k_accepted) / sizeof(k_accepted[0]));
        break;
    case FB_DUPLICATE:
        fb->steps = k_duplicate;
        fb->n_steps = (uint8_t)(sizeof(k_duplicate) / sizeof(k_duplicate[0]));
        break;
    case FB_UNKNOWN:
        fb->steps = k_unknown;
        fb->n_steps = (uint8_t)(sizeof(k_unknown) / sizeof(k_unknown[0]));
        break;
    case FB_LOW_BATTERY:
        fb->steps = s_lowbatt;
        fb->n_steps = (uint8_t)(sizeof(s_lowbatt) / sizeof(s_lowbatt[0]));
        break;
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
