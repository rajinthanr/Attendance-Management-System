/**
 * @file    em4100.c
 * @brief   Level 2 (logic) — EM4100 decoder implementation.
 *
 * Pipeline:
 *   edges -> intervals -> fitted half-bit clock -> half-bit symbols
 *         -> Manchester bits -> header search -> parity check -> tag
 *
 * The clock is fitted rather than assumed so the same code reads RF/64,
 * RF/32 and RF/16 tags and tolerates the antenna tank pulling the bit period
 * around by a few percent.
 */
#include "em4100.h"

/* A Manchester stream contains only two interval lengths, T and 2T. These
 * bound where the fitted T is allowed to land, in microseconds. RF/64 at
 * 125 kHz gives T = 256 us; RF/16 gives T = 64 us. The margin either side
 * absorbs tags whose internal oscillator is off nominal. */
#define T_MIN_US   40u
#define T_MAX_US   700u

/* Interval classification thresholds, in units of the fitted T. An interval
 * up to 1.5T is one half-bit, up to 2.5T is two, anything longer is a glitch
 * or a gap and terminates the run. Expressed in eighths to stay integer. */
#define THRESH_1_5T(t)  (((t) * 12u) / 8u)
#define THRESH_2_5T(t)  (((t) * 20u) / 8u)

/* ------------------------------------------------------------------------ */

/**
 * Fit the half-bit period to the interval population.
 *
 * A two-cluster 1-D k-means, seeded from the minimum interval. Two refinement
 * passes are enough: the clusters are separated by a factor of two, so the
 * seed is never in the wrong basin.
 */
static uint32_t fit_halfbit(const uint32_t *edges, uint16_t n_edges)
{
    uint32_t smallest = 0xFFFFFFFFu;
    uint16_t i;

    for (i = 1u; i < n_edges; i++) {
        uint32_t d = edges[i] - edges[i - 1u];   /* unsigned: wrap-safe */
        if (d != 0u && d < smallest) {
            smallest = d;
        }
    }
    if (smallest == 0xFFFFFFFFu) {
        return 0u;
    }

    /* Refine: average every interval that falls in the short cluster. */
    uint32_t estimate = smallest;
    uint8_t pass;

    for (pass = 0u; pass < 3u; pass++) {
        uint32_t cutoff = THRESH_1_5T(estimate);
        uint32_t sum = 0u;
        uint32_t count = 0u;

        for (i = 1u; i < n_edges; i++) {
            uint32_t d = edges[i] - edges[i - 1u];
            if (d != 0u && d <= cutoff) {
                sum += d;
                count++;
            }
        }
        if (count == 0u) {
            return 0u;
        }
        estimate = sum / count;
    }

    return estimate;
}

/**
 * Expand intervals into half-bit symbols.
 *
 * The absolute polarity of the first edge is unknown from timestamps alone,
 * so the level is simply assumed to start low and toggle. An inverted stream
 * yields inverted Manchester bits, which the caller compensates for by
 * searching the header in both polarities.
 */
static void build_halfbits(const uint32_t *edges,
                           uint16_t n_edges,
                           uint32_t t,
                           em4100_ws_t *ws)
{
    uint32_t one_hb = THRESH_1_5T(t);
    uint32_t two_hb = THRESH_2_5T(t);
    uint8_t level = 0u;
    uint16_t n = 0u;
    uint16_t i;

    for (i = 1u; i < n_edges && n < EM4100_MAX_HALFBITS; i++) {
        uint32_t d = edges[i] - edges[i - 1u];

        if (d <= one_hb) {
            ws->halfbits[n++] = level;
        } else if (d <= two_hb) {
            ws->halfbits[n++] = level;
            if (n < EM4100_MAX_HALFBITS) {
                ws->halfbits[n++] = level;
            }
        } else {
            /* Out-of-range interval: the run is broken. Everything captured
             * so far is still usable, so stop here rather than discarding. */
            break;
        }
        level ^= 1u;
    }

    ws->n_halfbits = n;
}

/**
 * Manchester-decode half-bits into bits at the alignment that produces the
 * longest clean run. A pair of equal half-bits is illegal Manchester and marks
 * the wrong alignment; there are only two candidates, so both are tried.
 */
static void decode_manchester(em4100_ws_t *ws)
{
    uint16_t best_offset = 0u;
    uint16_t best_valid = 0u;
    uint16_t offset;

    for (offset = 0u; offset < 2u; offset++) {
        uint16_t valid = 0u;
        uint16_t i;

        for (i = offset; (uint16_t)(i + 1u) < ws->n_halfbits; i += 2u) {
            if (ws->halfbits[i] == ws->halfbits[i + 1u]) {
                break;
            }
            valid++;
        }
        if (valid > best_valid) {
            best_valid = valid;
            best_offset = offset;
        }
    }

    /* Emit the full run at the winning alignment. A '10' half-bit pair is a
     * falling mid-bit transition, which EM4100 defines as a one. */
    uint16_t n = 0u;
    uint16_t i;

    for (i = best_offset; (uint16_t)(i + 1u) < ws->n_halfbits && n < EM4100_MAX_BITS; i += 2u) {
        if (ws->halfbits[i] == ws->halfbits[i + 1u]) {
            break;
        }
        ws->bits[n++] = (uint8_t)(ws->halfbits[i] & 1u);
    }

    ws->n_bits = n;
}

/* ------------------------------------------------------------------------ */

bool em4100_check_frame(const uint8_t *bits, app_tag_t *out)
{
    uint8_t i;
    uint8_t col[4] = { 0u, 0u, 0u, 0u };
    uint8_t nibble[10];

    /* Nine header ones. */
    for (i = 0u; i < 9u; i++) {
        if (bits[i] == 0u) {
            return false;
        }
    }

    /* Ten data rows, each four bits plus even row parity. */
    for (i = 0u; i < 10u; i++) {
        const uint8_t *row = &bits[9u + (i * 5u)];
        uint8_t value = 0u;
        uint8_t parity = 0u;
        uint8_t b;

        for (b = 0u; b < 4u; b++) {
            value = (uint8_t)((value << 1) | row[b]);
            parity ^= row[b];
            col[b] ^= row[b];
        }
        if (parity != row[4]) {
            return false;
        }
        nibble[i] = value;
    }

    /* Four even column parities. */
    for (i = 0u; i < 4u; i++) {
        if (col[i] != bits[59u + i]) {
            return false;
        }
    }

    /* Stop bit. */
    if (bits[63u] != 0u) {
        return false;
    }

    if (out != NULL) {
        out->version = (uint8_t)((nibble[0] << 4) | nibble[1]);
        out->unique_id = ((uint32_t)nibble[2] << 28) |
                         ((uint32_t)nibble[3] << 24) |
                         ((uint32_t)nibble[4] << 20) |
                         ((uint32_t)nibble[5] << 16) |
                         ((uint32_t)nibble[6] << 12) |
                         ((uint32_t)nibble[7] <<  8) |
                         ((uint32_t)nibble[8] <<  4) |
                         ((uint32_t)nibble[9]);
    }
    return true;
}

/**
 * Scan the bit stream for parity-clean frames and require two that agree.
 *
 * Two matching frames is what the flow chart asks for, and it is what makes a
 * misread practically impossible: a single frame already carries fourteen
 * parity bits, and an error would have to survive them twice and land on the
 * same wrong ID.
 */
static em4100_status_t find_confirmed_tag(em4100_ws_t *ws, app_tag_t *out)
{
    app_tag_t first;
    bool have_first = false;
    uint16_t i;

    ws->frames_found = 0u;

    if (ws->n_bits < EM4100_FRAME_BITS) {
        return EM4100_ERR_NO_SYNC;
    }

    for (i = 0u; (uint16_t)(i + EM4100_FRAME_BITS) <= ws->n_bits; i++) {
        app_tag_t tag;

        if (!em4100_check_frame(&ws->bits[i], &tag)) {
            continue;
        }

        ws->frames_found++;

        if (!have_first) {
            first = tag;
            have_first = true;
            /* Frames repeat back to back; the next one starts 64 bits on. */
            i += (EM4100_FRAME_BITS - 1u);
            continue;
        }

        if (tag.unique_id == first.unique_id && tag.version == first.version) {
            *out = tag;
            return EM4100_OK;
        }

        /* Disagreement means noise, not a second card. Restart confirmation
         * from this frame rather than giving up on the whole capture. */
        first = tag;
        i += (EM4100_FRAME_BITS - 1u);
    }

    return have_first ? EM4100_ERR_NO_CONFIRM : EM4100_ERR_NO_SYNC;
}

/** Flip every bit, so the header search can be retried in the other polarity. */
static void invert_bits(em4100_ws_t *ws)
{
    uint16_t i;
    for (i = 0u; i < ws->n_bits; i++) {
        ws->bits[i] ^= 1u;
    }
}

em4100_status_t em4100_decode(const uint32_t *edges,
                              uint16_t n_edges,
                              uint32_t tick_hz,
                              em4100_ws_t *ws,
                              app_tag_t *out)
{
    /* Two frames need 128 bits, so at least 128 edges even in the best case
     * where every bit is a single long interval. */
    if (n_edges < 130u || tick_hz == 0u) {
        return EM4100_ERR_TOO_FEW_EDGES;
    }
    if (n_edges > EM4100_MAX_EDGES) {
        n_edges = EM4100_MAX_EDGES;
    }

    uint32_t t = fit_halfbit(edges, n_edges);
    if (t == 0u) {
        return EM4100_ERR_NO_CLOCK;
    }

    /* Reject a fit that is not a physically plausible half-bit period. The
     * comparison is done in ticks to avoid a divide: t_us = t * 1e6 / tick_hz,
     * so t_us >= T_MIN_US becomes t * 1e6 >= T_MIN_US * tick_hz. Both sides
     * are kept inside 32 bits by scaling tick_hz down to MHz first. */
    uint32_t tick_mhz = tick_hz / 1000000u;
    if (tick_mhz == 0u) {
        tick_mhz = 1u;   /* sub-MHz timebase: fall back to a coarse check */
    }
    uint32_t t_us = t / tick_mhz;
    if (t_us < T_MIN_US || t_us > T_MAX_US) {
        return EM4100_ERR_NO_CLOCK;
    }
    ws->halfbit_ticks = t;

    build_halfbits(edges, n_edges, t, ws);
    decode_manchester(ws);

    em4100_status_t st = find_confirmed_tag(ws, out);
    if (st == EM4100_OK) {
        return st;
    }

    /* Retry with the opposite polarity: the capture never told us whether the
     * first edge was rising or falling. */
    invert_bits(ws);
    return find_confirmed_tag(ws, out);
}
