/**
 * @file    em4100.h
 * @brief   Level 2 (logic) — EM4100 / EM4102 frame decoder.
 *
 * Takes nothing but an array of edge timestamps and a tick rate, so it is
 * fully host-testable. No timer, no DMA, no HAL.
 *
 * Frame layout (64 bits, Manchester encoded at RF/64, RF/32 or RF/16):
 *
 *   9 x '1'            header
 *   10 x (D3 D2 D1 D0 PR)   ten data nibbles, each with even row parity
 *   PC3 PC2 PC1 PC0    even column parity over the ten nibbles
 *   '0'                stop bit
 *
 * The forty data bits are an 8-bit version/customer field followed by the
 * 32-bit unique ID.
 */
#ifndef EM4100_H
#define EM4100_H

#include "app_types.h"

/** Edge timestamps accepted per decode attempt (~3 frames at RF/64). */
#define EM4100_MAX_EDGES   512u

/** Half-bit symbols derived from those edges. */
#define EM4100_MAX_HALFBITS (EM4100_MAX_EDGES * 2u)

/** Manchester bits derived from those half-bits. */
#define EM4100_MAX_BITS    (EM4100_MAX_HALFBITS / 2u)

/** Bits in one complete frame. */
#define EM4100_FRAME_BITS  64u

typedef enum {
    EM4100_OK = 0,             /**< Two matching frames decoded. */
    EM4100_ERR_TOO_FEW_EDGES,  /**< Not enough signal to try. */
    EM4100_ERR_NO_CLOCK,       /**< Could not fit a plausible bit period. */
    EM4100_ERR_NO_SYNC,        /**< No header + parity-clean frame found. */
    EM4100_ERR_NO_CONFIRM      /**< One frame decoded but no matching second. */
} em4100_status_t;

/**
 * Scratch space for one decode. Supplied by the caller so the module keeps no
 * hidden state and the ~1.5 kB stays under the caller's control (the FSM
 * declares one static instance and reuses it).
 */
typedef struct {
    uint8_t halfbits[EM4100_MAX_HALFBITS];
    uint8_t bits[EM4100_MAX_BITS];
    uint16_t n_halfbits;
    uint16_t n_bits;
    uint32_t halfbit_ticks;  /**< Fitted half-bit period, in capture ticks. */
    uint8_t  frames_found;   /**< Parity-clean frames seen in this capture. */
} em4100_ws_t;

/**
 * Decode a capture.
 *
 * @param edges      Monotonic edge timestamps in capture ticks (both edges).
 * @param n_edges    Number of valid entries in @p edges.
 * @param tick_hz    Capture timebase, from plat_rf_capture_hz().
 * @param ws         Caller-owned scratch space, may hold garbage on entry.
 * @param out        Receives the tag on EM4100_OK.
 * @return           EM4100_OK, or the reason the capture was rejected.
 */
em4100_status_t em4100_decode(const uint32_t *edges,
                              uint16_t n_edges,
                              uint32_t tick_hz,
                              em4100_ws_t *ws,
                              app_tag_t *out);

/**
 * Validate and unpack one 64-bit frame starting at @p bits.
 * Exposed for the unit tests; @p bits must hold EM4100_FRAME_BITS entries.
 */
bool em4100_check_frame(const uint8_t *bits, app_tag_t *out);

#endif /* EM4100_H */
