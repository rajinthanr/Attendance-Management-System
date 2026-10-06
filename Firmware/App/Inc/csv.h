/**
 * @file    csv.h
 * @brief   Level 2 (logic) — binary record to ASCII CSV.
 *
 * The device records exactly two things for a tap: which card, and when. The
 * CSV says exactly that, and nothing else:
 *
 *   DATE,TIME,CARD_ID<spaces><CR><LF>
 *   2026-09-10,13:27:45,0000123456<CR><LF>
 *
 * Every row is exactly CSV_ROW_BYTES long, header included. That is the whole
 * trick behind the USB export: a mass-storage host reads 512-byte sectors in
 * whatever order it likes, and a fixed row length turns "which records does
 * sector N contain?" into a divide instead of a scan or a RAM-resident index.
 * 32 bytes divides 512 exactly: sixteen rows per sector, and no row ever
 * straddles a sector boundary, so generating a sector needs no state carried
 * from the last one.
 *
 * Columns: 0..9 date, 10 comma, 11..18 time, 19 comma, 20..29 card ID,
 * 30..31 CRLF. The ID is zero padded to ten digits for the same fixed-width
 * reason; a spreadsheet reads it back as text or as a plain number, and either
 * is the same card. Who the card belongs to is looked up on the PC, in the
 * student database, by this number.
 */
#ifndef CSV_H
#define CSV_H

#include "app_types.h"

/** 10 + 1 + 8 + 1 + 10 + CRLF. */
#define CSV_ROW_BYTES   32u

/** Rows in one 512-byte mass-storage sector. Exact, by construction. */
#define CSV_ROWS_PER_SECTOR  (512u / CSV_ROW_BYTES)

/** Write @p value as exactly @p width zero-padded decimal digits. */
void csv_put_padded(char *out, uint32_t value, uint8_t width);

/** Write the fixed header row. @p out must have CSV_ROW_BYTES of space. */
void csv_header(char *out);

/** Render one record. @p out must have CSV_ROW_BYTES of space. */
void csv_row(const app_record_t *rec, char *out);

/** Total CSV size for @p n_records, header included. */
uint32_t csv_size(uint32_t n_records);

#endif /* CSV_H */
