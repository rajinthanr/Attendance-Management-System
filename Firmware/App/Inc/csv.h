/**
 * @file    csv.h
 * @brief   Level 2 (logic) — binary record to ASCII CSV.
 *
 * Every row is exactly CSV_ROW_BYTES long, header included. That is the whole
 * trick behind the USB export: a mass-storage host reads 512-byte sectors in
 * whatever order it likes, and a fixed row length turns "which records does
 * sector N contain?" into a divide instead of a scan or a RAM-resident index.
 *
 * 32 bytes was chosen over any other fixed width because it divides 512
 * exactly: sixteen rows per sector, and no row ever straddles a sector
 * boundary, so generating a sector needs no state carried from the last one.
 *
 *   SCAN_DATE,SCAN_TIME,STUDENT_ID<CR><LF>
 *   2026-09-10,13:27:45,0000123456<CR><LF>
 *
 * The ID is zero padded to ten digits for the same fixed-width reason;
 * spreadsheets read it back as text, which is what a card number should be.
 */
#ifndef CSV_H
#define CSV_H

#include "app_types.h"

/** 10 + 1 + 8 + 1 + 10 + CRLF. */
#define CSV_ROW_BYTES   32u

/** Rows in one 512-byte mass-storage sector. Exact, by construction. */
#define CSV_ROWS_PER_SECTOR  (512u / CSV_ROW_BYTES)

/** Write the fixed header row. @p out must have CSV_ROW_BYTES of space. */
void csv_header(char *out);

/** Render one record. @p out must have CSV_ROW_BYTES of space. */
void csv_row(const app_record_t *rec, char *out);

/** Total CSV size for @p n_records, header included. */
uint32_t csv_size(uint32_t n_records);

#endif /* CSV_H */
