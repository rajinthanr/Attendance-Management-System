/**
 * @file    feedback.h
 * @brief   Level 2 (logic) — non-blocking LED / vibration sequencer.
 *
 * Patterns are tables of (output mask, duration) steps. Nothing here blocks,
 * so the MCU drops back into Stop 2 between steps: a 450 ms "unknown card"
 * buzz costs one wake per step instead of 450 ms of the core spinning in a
 * delay loop.
 */
#ifndef FEEDBACK_H
#define FEEDBACK_H

#include "app_types.h"

typedef enum {
    FB_NONE = 0,
    FB_ACCEPTED,    /**< Green LED + one short pulse. */
    FB_DUPLICATE,   /**< Two short green pulses with the motor. */
    FB_LOW_BATTERY, /**< Red LED blinks. */
    FB_ERROR,       /**< Red blinks with the motor: log full, reader fault. */
    FB_STATUS_OK,   /**< Two green blinks: battery fine (button tap). */
    FB_POWER_ON,    /**< Green with a short pulse. */
    FB_POWER_OFF,   /**< Red with a long pulse. */
    FB_SAVED,       /**< Green + two pulses: settings written to the device. */
    FB_REJECTED,    /**< Red + three pulses: settings file refused. */
    FB_HOLD,        /**< One short pulse: the button has been held long enough for a new lecture. */
    FB_LECTURE      /**< Green + three short pulses: a new lecture has started. */
} fb_pattern_t;

typedef struct {
    uint32_t outputs;   /**< PLAT_OUT_* bitmask for this step. */
    uint16_t ms;        /**< How long to hold it. */
} fb_step_t;

typedef struct {
    const fb_step_t *steps;
    uint8_t n_steps;
    uint8_t index;
    bool    active;
} feedback_t;

void fb_init(feedback_t *fb);

/**
 * Begin @p pattern. Applies the first step's outputs and returns the delay
 * the caller should arm its one-shot timer with; 0 means nothing to do.
 */
uint16_t fb_start(feedback_t *fb, fb_pattern_t pattern);

/**
 * Advance one step, called when the one-shot timer fires.
 * @return the next delay in ms, or 0 when the pattern is finished (at which
 *         point all outputs have been turned off).
 */
uint16_t fb_advance(feedback_t *fb);

bool fb_is_active(const feedback_t *fb);

/** Abort immediately and clear all outputs. */
void fb_cancel(feedback_t *fb);

#endif /* FEEDBACK_H */
