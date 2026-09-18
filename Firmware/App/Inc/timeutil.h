/**
 * @file    timeutil.h
 * @brief   Level 2 (logic) — calendar <-> epoch conversion.
 *
 * The RTC hands Level 1 a set of BCD calendar fields and nothing else. Every
 * piece of date arithmetic in the product lives here.
 */
#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include "app_types.h"

/** Calendar -> seconds since 2000-01-01T00:00:00Z. */
app_epoch_t time_to_epoch(const app_datetime_t *dt);

/** Seconds since 2000-01-01T00:00:00Z -> calendar. */
void time_from_epoch(app_epoch_t epoch, app_datetime_t *out);

/** True if @p dt is a self-consistent date and time. */
bool time_is_valid(const app_datetime_t *dt);

/** Days in @p month of @p year, handling February. */
uint8_t time_days_in_month(uint16_t year, uint8_t month);

/** Proleptic Gregorian leap year test. */
bool time_is_leap(uint16_t year);

#endif /* TIMEUTIL_H */
