/**
 * @file    session.c
 * @brief   Level 2 (logic) — session markers and the in-session duplicate check.
 */
#include "session.h"

bool sess_is_marker(uint32_t id)
{
    return id >= NV_ID_RESERVED_MIN;
}

bool sess_is_header(uint32_t id)
{
    return (id & 0xFFFFFFF0u) == NV_MARK_HEADER;
}

uint32_t sess_span(uint32_t header_id)
{
    return 1u + (header_id & 0x0Fu);
}

/** Copy at most @p max bytes of @p s, never splitting a UTF-8 character. */
static uint32_t copy_name(uint8_t *dst, const char *s, uint32_t max)
{
    uint32_t n = 0u;

    if (s == NULL) {
        return 0u;
    }
    while (s[n] != '\0' && n < max) {
        n++;
    }
    /* If the next byte is a continuation byte the cut landed mid-character. */
    if (s[n] != '\0' && ((uint8_t)s[n] & 0xC0u) == 0x80u) {
        while (n > 0u && ((uint8_t)s[n - 1u] & 0xC0u) == 0x80u) {
            n--;
        }
        if (n > 0u) {
            n--;
        }
    }
    {
        uint32_t i;

        for (i = 0u; i < n; i++) {
            dst[i] = (uint8_t)s[i];
        }
    }
    return n;
}

uint16_t sess_encode(app_record_t *out, uint16_t cap, app_epoch_t start,
                     const char *module, const char *lecture)
{
    uint8_t text[SESS_MODULE_MAX + SESS_LECTURE_MAX + 2u];
    uint32_t len = 0u;
    uint32_t k, i;

    len += copy_name(&text[len], module, SESS_MODULE_MAX);
    text[len++] = 0u;
    len += copy_name(&text[len], lecture, SESS_LECTURE_MAX);
    text[len++] = 0u;

    k = (len + 3u) / 4u;                    /* text records, 4 bytes each */
    if (cap < (uint16_t)(1u + k)) {
        return 0u;
    }

    out[0].student_id = NV_MARK_HEADER | k;
    out[0].stamp = start;

    for (i = 0u; i < k; i++) {
        uint32_t w = 0u;
        uint32_t b;

        for (b = 0u; b < 4u; b++) {
            uint32_t at = (i * 4u) + b;

            if (at < len) {
                w |= (uint32_t)text[at] << (8u * b);
            }
        }
        out[1u + i].student_id = NV_MARK_TEXT;
        out[1u + i].stamp = w;
    }
    return (uint16_t)(1u + k);
}

bool sess_decode(const app_record_t *recs, uint16_t n, session_t *out)
{
    uint8_t text[SESS_MAX_RECORDS * 4u];
    uint32_t k, i, len, m, l;

    out->valid = false;
    out->start = 0u;
    out->module[0] = '\0';
    out->lecture[0] = '\0';

    if (n == 0u || !sess_is_header(recs[0].student_id)) {
        return false;
    }
    k = recs[0].student_id & 0x0Fu;
    if ((1u + k) > n) {
        return false;
    }

    for (i = 0u; i < k; i++) {
        uint32_t b;

        if (recs[1u + i].student_id != NV_MARK_TEXT) {
            return false;
        }
        for (b = 0u; b < 4u; b++) {
            text[(i * 4u) + b] = (uint8_t)((recs[1u + i].stamp >> (8u * b)) & 0xFFu);
        }
    }
    len = k * 4u;

    m = 0u;
    while (m < len && text[m] != 0u) {
        m++;
    }
    l = 0u;
    if (m < len) {
        while ((m + 1u + l) < len && text[m + 1u + l] != 0u) {
            l++;
        }
    }

    for (i = 0u; i < m && i < SESS_MODULE_MAX; i++) {
        out->module[i] = (char)text[i];
    }
    out->module[i] = '\0';
    for (i = 0u; i < l && i < SESS_LECTURE_MAX; i++) {
        out->lecture[i] = (char)text[m + 1u + i];
    }
    out->lecture[i] = '\0';
    out->start = recs[0].stamp;
    out->valid = true;
    return true;
}

bool sess_read(const log_store_t *ls, uint32_t index, session_t *out)
{
    app_record_t recs[SESS_MAX_RECORDS];
    uint32_t span, n;

    out->valid = false;
    if (!log_read(ls, index, &recs[0]) || !sess_is_header(recs[0].student_id)) {
        return false;
    }
    span = sess_span(recs[0].student_id);
    for (n = 1u; n < span; n++) {
        if (!log_read(ls, index + n, &recs[n])) {
            return false;
        }
    }
    return sess_decode(recs, (uint16_t)span, out);
}

/** Age of @p stamp at @p now, treating a stamp from the future as brand new. */
static uint32_t age_of(app_epoch_t stamp, app_epoch_t now)
{
    return (stamp > now) ? 0u : (now - stamp);
}

bool sess_card_seen(const log_store_t *ls, const record_buffer_t *rb, uint32_t id,
                    app_epoch_t now, uint32_t max_age_s, uint32_t max_scan)
{
    uint32_t scanned = 0u;
    uint32_t i;

    /* Newest first: the RAM buffer holds the most recent records. */
    if (rb != NULL) {
        for (i = rb_count(rb); i > 0u && scanned < max_scan; i--, scanned++) {
            const app_record_t *r = rb_peek(rb, (uint16_t)(i - 1u));

            if (r == NULL) {
                break;
            }
            if (sess_is_header(r->student_id)) {
                return false;               /* an earlier lecture */
            }
            if (sess_is_marker(r->student_id)) {
                continue;
            }
            if (age_of(r->stamp, now) > max_age_s) {
                return false;
            }
            if (r->student_id == id) {
                return true;
            }
        }
    }

    if (ls != NULL) {
        for (i = log_total(ls); i > 0u && scanned < max_scan; i--, scanned++) {
            app_record_t r;

            if (!log_read(ls, i - 1u, &r)) {
                break;
            }
            if (sess_is_header(r.student_id)) {
                return false;
            }
            if (sess_is_marker(r.student_id)) {
                continue;
            }
            if (age_of(r.stamp, now) > max_age_s) {
                return false;
            }
            if (r.student_id == id) {
                return true;
            }
        }
    }
    return false;
}
