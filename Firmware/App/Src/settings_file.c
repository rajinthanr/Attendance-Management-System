/**
 * @file    settings_file.c
 * @brief   Level 2 (logic) — SETTINGS.CSV render and parse.
 */
#include "settings_file.h"
#include "csv.h"
#include "timeutil.h"

#define ID_ERASED    0xFFFFFFFFu
#define KEY_MAX      24u    /* longest directive keyword kept */

/* ------------------------------------------------------------------------ */
/* Render                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    bool     overflow;
} out_t;

static void out_char(out_t *o, char c)
{
    if (o->len < o->cap) {
        o->buf[o->len] = c;
    } else {
        o->overflow = true;
    }
    o->len++;
}

static void out_str(out_t *o, const char *s)
{
    while (*s != '\0') {
        out_char(o, *s++);
    }
}

static void out_padded(out_t *o, uint32_t v, uint8_t width)
{
    char tmp[10];
    uint8_t i;

    csv_put_padded(tmp, v, width);
    for (i = 0u; i < width; i++) {
        out_char(o, tmp[i]);
    }
}

static void out_eol(out_t *o)
{
    out_char(o, '\r');
    out_char(o, '\n');
}

/** A directive value, made safe: no comma, quote or control character. */
static void out_value(out_t *o, const char *s)
{
    uint32_t i;

    for (i = 0u; s != NULL && s[i] != '\0'; i++) {
        char c = s[i];
        bool bad = ((unsigned char)c < 0x20u) || (c == ',') || (c == '"') ||
                   ((unsigned char)c == 0x7Fu);

        out_char(o, bad ? ' ' : c);
    }
}

uint32_t setf_render(char *buf, uint32_t cap, const app_datetime_t *now,
                     const char *module, const char *lecture, uint32_t device_id)
{
    out_t o = { buf, cap, 0u, false };

    out_str(&o, "# Edit these lines, then eject the drive (or press the button). "
                "Add #NEWSESSION,1 to start another lecture with the same names.");
    out_eol(&o);

    out_str(&o, "#TIME,");
    out_padded(&o, now->year, 4u);
    out_char(&o, '-');
    out_padded(&o, now->month, 2u);
    out_char(&o, '-');
    out_padded(&o, now->day, 2u);
    out_char(&o, ' ');
    out_padded(&o, now->hour, 2u);
    out_char(&o, ':');
    out_padded(&o, now->minute, 2u);
    out_char(&o, ':');
    out_padded(&o, now->second, 2u);
    out_eol(&o);

    out_str(&o, "#MODULE,");
    out_value(&o, module);
    out_eol(&o);
    out_str(&o, "#LECTURE,");
    out_value(&o, lecture);
    out_eol(&o);

    if (device_id != 0u) {
        out_str(&o, "#DEVICE,");
        out_padded(&o, device_id, 10u);
        out_eol(&o);
    }

    return o.overflow ? 0u : o.len;
}

/* ------------------------------------------------------------------------ */
/* Line parsing                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    char     key[KEY_MAX + 1u];      /**< Field 0, e.g. "#TIME". */
    uint32_t key_len;

    char     val[SESS_LECTURE_MAX + 1u];   /**< Field 1: the widest value, a lecture. */
    uint32_t val_len;
    bool     val_truncated;
    uint8_t  first_dropped;          /**< First byte that did not fit, for the UTF-8 fix. */

    bool     any_text;               /**< Anything but spaces and commas seen. */
    bool     directive;              /**< First non-blank character was '#'. */
} line_t;

static bool is_space(uint8_t c)
{
    return (c == ' ') || (c == '\t');
}

/**
 * Finish the value: trim trailing spaces, and if the cut fell inside a UTF-8
 * sequence remove the partial character so the stored text stays valid.
 */
static void val_finish(line_t *l)
{
    if (l->val_truncated && (l->first_dropped & 0xC0u) == 0x80u) {
        while (l->val_len > 0u && ((uint8_t)l->val[l->val_len - 1u] & 0xC0u) == 0x80u) {
            l->val_len--;
        }
        if (l->val_len > 0u) {
            l->val_len--;       /* the lead byte of the split character */
        }
    }
    while (l->val_len > 0u && l->val[l->val_len - 1u] == ' ') {
        l->val_len--;
    }
    l->val[l->val_len] = '\0';
}

/**
 * Read one line starting at @p *pos and leave @p *pos on the next.
 * Fields are split on commas outside quotes; field 0 is the keyword and
 * field 1 the value; the rest are skipped.
 * @return false when there is no more input.
 */
static bool read_line(setf_get_fn get, void *ctx, uint32_t size, uint32_t *pos_io, line_t *l)
{
    uint32_t pos = *pos_io;
    uint32_t field = 0u;
    bool in_quotes = false;
    bool field_empty = true;     /* only whitespace so far in this field */
    bool first_char = true;
    bool key_overflow = false;
    int c;

    l->key_len = 0u;
    l->val_len = 0u;
    l->val_truncated = false;
    l->first_dropped = 0u;
    l->any_text = false;
    l->directive = false;

    if (pos >= size) {
        return false;
    }

    /* UTF-8 byte-order mark: a spreadsheet's "CSV UTF-8" save puts one here. */
    if (pos == 0u && size >= 3u && get(ctx, 0u) == 0xEF && get(ctx, 1u) == 0xBB &&
        get(ctx, 2u) == 0xBF) {
        pos = 3u;
    }

    for (;;) {
        c = (pos < size) ? get(ctx, pos) : -1;
        if (c < 0) {
            break;
        }
        pos++;

        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            if (pos < size && get(ctx, pos) == '\n') {
                pos++;
            }
            break;
        }

        if (first_char && !is_space((uint8_t)c)) {
            first_char = false;
            if (c == '#') {
                l->directive = true;
            }
        }

        if (in_quotes) {
            if (c == '"') {
                if (pos < size && get(ctx, pos) == '"') {
                    pos++;                      /* "" is a literal quote */
                    c = ' ';                    /* ...which a value cannot hold */
                } else {
                    in_quotes = false;
                    continue;
                }
            } else if (c == ',') {
                c = ' ';                        /* a comma inside quotes is data */
            }
        } else {
            if (c == '"' && field_empty) {
                in_quotes = true;
                field_empty = false;
                continue;
            }
            if (c == ',') {
                field++;
                field_empty = true;
                continue;
            }
        }

        if (c == '\t' || (uint8_t)c < 0x20u || c == 0x7F) {
            c = ' ';
        }
        if (c == ' ' && field_empty) {
            continue;                           /* leading whitespace */
        }

        field_empty = false;
        l->any_text = true;

        if (field == 0u) {
            if (l->key_len < KEY_MAX) {
                l->key[l->key_len++] = (char)c;
            } else {
                key_overflow = true;
            }
        } else if (field == 1u) {
            if (l->val_len < SESS_LECTURE_MAX) {
                l->val[l->val_len++] = (char)c;
            } else if (!l->val_truncated && c != ' ') {
                l->val_truncated = true;        /* spaces past the limit are just padding */
                l->first_dropped = (uint8_t)c;
            }
        }
    }

    while (l->key_len > 0u && l->key[l->key_len - 1u] == ' ') {
        l->key_len--;
    }
    if (key_overflow) {
        l->key_len = 0u;                        /* too long to be any directive */
    }
    l->key[l->key_len] = '\0';
    val_finish(l);

    *pos_io = pos;
    return true;
}

/**
 * Copy @p len bytes of @p src into @p dst, at most @p max of them, without
 * splitting a UTF-8 character and without trailing spaces. @p dst needs
 * max + 1 bytes.
 */
static void copy_limited(char *dst, const char *src, uint32_t len, uint32_t max)
{
    uint32_t n = (len > max) ? max : len;
    uint32_t i;

    /* If the byte after the cut is a continuation byte, back up over the
     * partial character, lead byte included. */
    if (len > n && ((uint8_t)src[n] & 0xC0u) == 0x80u) {
        while (n > 0u && ((uint8_t)src[n - 1u] & 0xC0u) == 0x80u) {
            n--;
        }
        if (n > 0u) {
            n--;
        }
    }
    while (n > 0u && src[n - 1u] == ' ') {
        n--;
    }
    for (i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

/** Parse decimal or 0x-hex into a 32-bit value. No sign, no overflow. */
static bool parse_u32(const char *s, uint32_t len, uint32_t *out)
{
    uint64_t v = 0u;
    uint32_t i = 0u;
    uint32_t base = 10u;

    if (len == 0u) {
        return false;
    }
    if (len > 2u && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16u;
        i = 2u;
    }
    for (; i < len; i++) {
        char c = s[i];
        uint32_t d;

        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (base == 16u && c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10u;
        } else if (base == 16u && c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A') + 10u;
        } else {
            return false;
        }
        v = (v * base) + d;
        if (v > 0xFFFFFFFFull) {
            return false;
        }
    }
    *out = (uint32_t)v;
    return true;
}

/** Case-insensitive match of a directive keyword such as "#TIME". */
static bool keyword(const line_t *l, const char *kw)
{
    uint32_t i = 0u;

    for (; kw[i] != '\0'; i++) {
        char c = l->key[i];

        if (i >= l->key_len) {
            return false;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != kw[i]) {
            return false;
        }
    }
    return i == l->key_len;
}

/**
 * Read 5 or 6 numbers separated by anything that is not a digit:
 * year month day hour minute [second]. Accepts "2026-10-06 14:30:00",
 * "2026-10-06T14:30" and "2026/10/6 9:05".
 */
static bool parse_time(const char *s, uint32_t len, app_datetime_t *dt)
{
    uint32_t v[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
    uint32_t digits[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
    uint32_t n = 0u;
    bool in_num = false;
    uint32_t i;

    for (i = 0u; i < len; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (!in_num) {
                if (n >= 6u) {
                    return false;
                }
                in_num = true;
                n++;
            }
            if (digits[n - 1u] >= 4u) {
                return false;
            }
            v[n - 1u] = (v[n - 1u] * 10u) + (uint32_t)(s[i] - '0');
            digits[n - 1u]++;
        } else {
            in_num = false;
        }
    }

    if (n < 5u || digits[0] != 4u) {
        return false;
    }
    /* Check the ranges before narrowing, or a huge field could wrap into range. */
    if (v[1] > 99u || v[2] > 99u || v[3] > 99u || v[4] > 99u || v[5] > 99u) {
        return false;
    }

    dt->year = (uint16_t)v[0];
    dt->month = (uint8_t)v[1];
    dt->day = (uint8_t)v[2];
    dt->hour = (uint8_t)v[3];
    dt->minute = (uint8_t)v[4];
    dt->second = (n == 6u) ? (uint8_t)v[5] : 0u;
    return time_is_valid(dt);
}

/* ------------------------------------------------------------------------ */
/* Scan                                                                     */
/* ------------------------------------------------------------------------ */

void setf_scan(setf_get_fn get, void *ctx, uint32_t size, setf_report_t *rep)
{
    uint32_t pos = 0u;
    line_t l;

    rep->status = SETF_OK;
    rep->has_time = false;
    rep->has_device = false;
    rep->device_id = 0u;
    rep->bad_directive = false;
    rep->has_module = false;
    rep->has_lecture = false;
    rep->new_session = false;
    rep->clear_log = false;
    rep->module[0] = '\0';
    rep->lecture[0] = '\0';

    if (size == 0u) {
        rep->status = SETF_ERR_EMPTY;
        return;
    }
    if (size > SETF_MAX_BYTES) {
        rep->status = SETF_ERR_TOO_LARGE;
        return;
    }

    while (read_line(get, ctx, size, &pos, &l)) {
        /* Only directives mean anything; every other line is ignored. */
        if (!l.any_text || !l.directive) {
            continue;
        }

        if (keyword(&l, "#TIME")) {
            app_datetime_t dt;

            if (parse_time(l.val, l.val_len, &dt)) {
                rep->has_time = true;
                rep->time = dt;
            } else {
                rep->bad_directive = true;
            }
        } else if (keyword(&l, "#DEVICE")) {
            uint32_t dev;

            if (parse_u32(l.val, l.val_len, &dev) && dev != ID_ERASED) {
                rep->has_device = true;
                rep->device_id = dev;
            } else {
                rep->bad_directive = true;
            }
        } else if (keyword(&l, "#MODULE")) {
            rep->has_module = true;
            copy_limited(rep->module, l.val, l.val_len, SESS_MODULE_MAX);
        } else if (keyword(&l, "#LECTURE")) {
            rep->has_lecture = true;
            copy_limited(rep->lecture, l.val, l.val_len, SESS_LECTURE_MAX);
        } else if (keyword(&l, "#NEWSESSION")) {
            rep->new_session = true;
        } else if (keyword(&l, "#CLEARLOG")) {
            rep->clear_log = true;
        }
        /* Any other '#' line is a comment, an old app's #CARDS included. */
    }
}
