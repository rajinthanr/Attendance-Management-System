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

void csv_row(const app_record_t *rec, char *out)
{
    app_datetime_t dt;

    time_from_epoch(rec->stamp, &dt);

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

    /* Card ID, ten digits: the widest a 32-bit value can be. */
    csv_put_padded(&out[20], rec->student_id, 10u);

    out[CSV_ROW_BYTES - 2u] = '\r';
    out[CSV_ROW_BYTES - 1u] = '\n';
}

uint32_t csv_size(uint32_t n_records)
{
    return (n_records + 1u) * CSV_ROW_BYTES;
}
