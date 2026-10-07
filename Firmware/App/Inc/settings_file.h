/**
 * @file    settings_file.h
 * @brief   Level 2 (logic) — SETTINGS.CSV: the few things the PC tells the device.
 *
 * The device records only card IDs and time stamps. Who owns a card, which
 * module they take and what department they are in is the PC's business and
 * lives in its database. This file carries what the device itself needs:
 *
 *   # Edit these lines, then eject the drive (or press the button). ...
 *   #TIME,2026-10-06 14:30:00
 *   #MODULE,EN2090
 *   #LECTURE,Circuits Lecture 4
 *   #DEVICE,0000012345
 *
 * There is no card list: the device records every card, and the PC app alone
 * decides which ones are registered.
 *
 * Rules (checked before anything is applied):
 *   - CRLF, LF or lone CR line ends; a UTF-8 byte-order mark is skipped.
 *   - Only lines whose first non-blank character is '#' mean anything. Every
 *     other line is ignored, so an old file that still lists students, or a
 *     spreadsheet's stray rows, can do no harm.
 *   - "#TIME,YYYY-MM-DD HH:MM[:SS]" sets the clock, but only when it differs
 *     from the time the file was shown with, so an untouched file never winds
 *     the clock back to the moment of attach. A malformed value is reported
 *     and ignored.
 *   - "#DEVICE,<id>" sets the device ID (decimal, or 0x hex).
 *   - "#MODULE,<name>" and "#LECTURE,<name>" name the lecture session that the
 *     next taps belong to (24 and 32 bytes). A new session starts when either
 *     differs from what the file was shown with, or when a "#NEWSESSION" line
 *     is present (a second lecture with the same names).
 *   - "#CLEARLOG" erases every record and lecture marker in the log. The
 *     companion app sends it with a new lecture, once it has imported the
 *     taps; renaming the lecture alone never clears anything.
 *   - A comma or quote inside a name becomes a space; quoted values work.
 *   - Any other '#' line is a comment. That includes "#CARDS" from an older
 *     app: it and the numbers after it are ignored.
 */
#ifndef SETTINGS_FILE_H
#define SETTINGS_FILE_H

#include "app_types.h"
#include "session.h"
#include "nv_layout.h"

/** Most bytes of settings text the device will look at (also the USB window). */
#define SETF_MAX_BYTES   (28u * 512u)

typedef enum {
    SETF_OK = 0,
    SETF_ERR_EMPTY,       /**< A zero-length file: the host is mid-copy, or made a blank one. */
    SETF_ERR_TOO_LARGE,   /**< Bigger than SETF_MAX_BYTES. */
    SETF_ERR_FILE,        /**< The host's file system image was unusable. */
    SETF_ERR_FLASH        /**< Storing the device ID failed. */
} setf_status_t;

/** Byte source: the byte at @p offset, or -1 at or past the end. */
typedef int (*setf_get_fn)(void *ctx, uint32_t offset);

typedef struct {
    setf_status_t status;

    bool     has_time;      /**< A well-formed #TIME line was present. */
    app_datetime_t time;
    bool     has_device;    /**< A well-formed #DEVICE line was present. */
    uint32_t device_id;
    bool     bad_directive; /**< A #TIME or #DEVICE value was malformed. */

    bool     has_module;    /**< A #MODULE line was present (it may be empty). */
    bool     has_lecture;   /**< A #LECTURE line was present. */
    bool     new_session;   /**< A #NEWSESSION line was present. */
    bool     clear_log;     /**< A #CLEARLOG line was present. */
    char     module[SESS_MODULE_MAX + 1u];
    char     lecture[SESS_LECTURE_MAX + 1u];
} setf_report_t;

/**
 * Render the file shown to the host.
 * @param now        Printed on the #TIME line.
 * @param module     Current session module (NULL = empty).
 * @param lecture    Current session lecture.
 * @param device_id  Printed on #DEVICE when non-zero.
 * @return bytes written, or 0 if @p cap is too small.
 */
uint32_t setf_render(char *buf, uint32_t cap, const app_datetime_t *now,
                     const char *module, const char *lecture, uint32_t device_id);

/** Parse @p size bytes. Never touches flash; always fills @p rep. */
void setf_scan(setf_get_fn get, void *ctx, uint32_t size, setf_report_t *rep);

#endif /* SETTINGS_FILE_H */
