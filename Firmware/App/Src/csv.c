/**
 * @file    csv.c
 * @brief   Level 2 (logic) — CSV rendering.
 *
 * Hand-rolled rather than snprintf: the printf family costs several kilobytes
 * of flash and drags in a heap, and this only ever needs zero-padded decimal.
 */
#include "csv.h"
#include "timeutil.h"

static const char k_header[] = "DATE,TIME,CARD_ID";
static const char k_lecture_header[] = "DATE,TIME,MODULE,LECTURE";

void csv_put_padded(char *out, uint32_t value, uint8_t width)
{
    uint8_t i;

    for (i = width; i > 0u; i--) {
        out[i - 1u] = (char)('0' + (value % 10u));
        value /= 10u;
    }
}

void csv_header(char *out)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)(sizeof(k_header) - 1u); i++) {
        out[i] = k_header[i];
    }
    for (; i < (uint8_t)(CSV_ROW_BYTES - 2u); i++) {
        out[i] = ' ';
    }
    out[CSV_ROW_BYTES - 2u] = '\r';
    out[CSV_ROW_BYTES - 1u] = '\n';
}

/** "YYYY-MM-DD,HH:MM:SS," into out[0..19]. */
static void put_stamp(app_epoch_t stamp, char *out)
{
    app_datetime_t dt;

    time_from_epoch(stamp, &dt);

    /* YYYY-MM-DD */
    csv_put_padded(&out[0], dt.year, 4u);
    out[4] = '-';
    csv_put_padded(&out[5], dt.month, 2u);
    out[7] = '-';
    csv_put_padded(&out[8], dt.day, 2u);
    out[10] = ',';

    /* HH:MM:SS */
    csv_put_padded(&out[11], dt.hour, 2u);
    out[13] = ':';
    csv_put_padded(&out[14], dt.minute, 2u);
    out[16] = ':';
    csv_put_padded(&out[17], dt.second, 2u);
    out[19] = ',';
}

void csv_row(const app_record_t *rec, char *out)
{
    put_stamp(rec->stamp, out);

    /* Card ID, ten digits: the widest a 32-bit value can be. */
    csv_put_padded(&out[20], rec->student_id, 10u);

    out[CSV_ROW_BYTES - 2u] = '\r';
    out[CSV_ROW_BYTES - 1u] = '\n';
}

uint32_t csv_size(uint32_t n_records)
{
    return (n_records + 1u) * CSV_ROW_BYTES;
}

/** Copy at most @p max bytes of @p s (NULL is empty); returns the bytes written. */
static uint32_t put_text(char *out, const char *s, uint32_t max)
{
    uint32_t n = 0u;

    while (s != NULL && n < max && s[n] != '\0') {
        out[n] = s[n];
        n++;
    }
    return n;
}

static void pad_row(char *out, uint32_t from)
{
    uint32_t i;

    for (i = from; i < (CSV_LECTURE_ROW_BYTES - 2u); i++) {
        out[i] = ' ';
    }
    out[CSV_LECTURE_ROW_BYTES - 2u] = '\r';
    out[CSV_LECTURE_ROW_BYTES - 1u] = '\n';
}

void csv_lecture_header(char *out)
{
    pad_row(out, put_text(out, k_lecture_header, CSV_LECTURE_ROW_BYTES - 2u));
}

void csv_lecture_row(app_epoch_t start, const char *module, const char *lecture, char *out)
{
    uint32_t n = 20u;

    put_stamp(start, out);
    n += put_text(&out[n], module, 24u);
    out[n++] = ',';
    n += put_text(&out[n], lecture, 32u);
    pad_row(out, n);
}

uint32_t csv_lecture_size(uint32_t n_lectures)
{
    return (n_lectures + 1u) * CSV_LECTURE_ROW_BYTES;
}
