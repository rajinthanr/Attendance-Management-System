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

/*
 * LECTURES.CSV: one row per lecture start in the log, the same fixed-width
 * idea with wider rows, since the names do not fit in 32 bytes:
 *
 *   DATE,TIME,MODULE,LECTURE<spaces><CR><LF>
 *   2026-10-07,14:00:00,EN2090,Circuits Lecture 5<spaces><CR><LF>
 *
 * 10 + 1 + 8 + 1 + module (24) + 1 + lecture (32) = 77 bytes of text at most,
 * space padded to 126, then CRLF. Four rows per sector. Neither name can hold
 * a comma or a quote (SETTINGS.CSV refuses them), so no quoting is needed.
 */
#define CSV_LECTURE_ROW_BYTES        128u
#define CSV_LECTURE_ROWS_PER_SECTOR  (512u / CSV_LECTURE_ROW_BYTES)

/** Write the LECTURES.CSV header row. @p out must have CSV_LECTURE_ROW_BYTES of space. */
void csv_lecture_header(char *out);

/** Render one lecture start; names are cut at 24 and 32 bytes, NULL is empty. */
void csv_lecture_row(app_epoch_t start, const char *module, const char *lecture, char *out);

/** LECTURES.CSV size for @p n_lectures, header included. */
uint32_t csv_lecture_size(uint32_t n_lectures);

#endif /* CSV_H */
