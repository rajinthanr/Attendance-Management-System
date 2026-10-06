/**
 * @file    session.h
 * @brief   Level 2 (logic) — lecture sessions in the attendance log, and the
 *          "has this card already signed in?" check that relies on them.
 *
 * A session is a module name and a lecture name that apply to every attendance
 * record after its marker, up to the next marker. The marker format is in
 * nv_layout.h. Markers are never exported as rows: ATTEND.CSV shows the names
 * in two columns instead.
 */
#ifndef SESSION_H
#define SESSION_H

#include "app_types.h"
#include "nv_layout.h"
#include "log_store.h"
#include "record_buffer.h"

#define SESS_MODULE_MAX    24u
#define SESS_LECTURE_MAX   32u
/** Most records one marker can occupy: the header and 15 text records. */
#define SESS_MAX_RECORDS   16u

typedef struct {
    bool        valid;
    app_epoch_t start;
    char        module[SESS_MODULE_MAX + 1u];
    char        lecture[SESS_LECTURE_MAX + 1u];
} session_t;

/** True for any record that is a marker rather than an attendance record. */
bool sess_is_marker(uint32_t id);

/** True for the first record of a marker. */
bool sess_is_header(uint32_t id);

/** Records a marker occupies, from its header's id (1 + text records). */
uint32_t sess_span(uint32_t header_id);

/**
 * Build a marker.
 * @param out  Room for SESS_MAX_RECORDS records.
 * @return records written, or 0 if @p cap is too small. Names longer than the
 *         limits are cut; NULL is treated as empty.
 */
uint16_t sess_encode(app_record_t *out, uint16_t cap, app_epoch_t start,
                     const char *module, const char *lecture);

/** Decode a marker from @p n consecutive records starting at its header. */
bool sess_decode(const app_record_t *recs, uint16_t n, session_t *out);

/** Read the marker whose header is log record @p index. */
bool sess_read(const log_store_t *ls, uint32_t index, session_t *out);

/**
 * Has @p id already been recorded in the current session?
 *
 * Scans the RAM buffer and then the flash log backwards from the newest
 * record, and stops at the first session header, at a record older than
 * @p max_age_s, or after @p max_scan records, whichever comes first. The
 * bounds make this cheap, and make it work with no session set (a card seen
 * in the last few hours is still a duplicate) and with a clock that has been
 * set backwards.
 *
 * Reads flash, not RAM-only state, so it keeps working after Standby.
 */
bool sess_card_seen(const log_store_t *ls, const record_buffer_t *rb, uint32_t id,
                    app_epoch_t now, uint32_t max_age_s, uint32_t max_scan);

#endif /* SESSION_H */
