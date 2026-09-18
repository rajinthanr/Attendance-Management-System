/**
 * @file    timeutil.c
 * @brief   Level 2 (logic) — calendar arithmetic.
 *
 * Uses the days-from-civil algorithm: a closed form with no loops and no
 * lookup of cumulative month lengths, which keeps it constant time and easy
 * to test exhaustively on the host.
 */
#include "timeutil.h"
#include "app_config.h"

#define SECS_PER_DAY   86400u
#define SECS_PER_HOUR  3600u
#define SECS_PER_MIN   60u

/** Days from 1970-01-01 to 2000-01-01, the offset our epoch is shifted by. */
#define DAYS_1970_TO_2000  10957

bool time_is_leap(uint16_t year)
{
    return ((year % 4u) == 0u) && (((year % 100u) != 0u) || ((year % 400u) == 0u));
}

uint8_t time_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t len[12] = { 31u, 28u, 31u, 30u, 31u, 30u,
                                     31u, 31u, 30u, 31u, 30u, 31u };
    if (month < 1u || month > 12u) {
        return 0u;
    }
    if (month == 2u && time_is_leap(year)) {
        return 29u;
    }
    return len[month - 1u];
}

bool time_is_valid(const app_datetime_t *dt)
{
    if (dt == NULL) {
        return false;
    }
    if (dt->year < (uint16_t)APP_EPOCH_YEAR || dt->year > 2099u) {
        return false;
    }
    if (dt->month < 1u || dt->month > 12u) {
        return false;
    }
    if (dt->day < 1u || dt->day > time_days_in_month(dt->year, dt->month)) {
        return false;
    }
    return (dt->hour <= 23u) && (dt->minute <= 59u) && (dt->second <= 59u);
}

/**
 * Days since 1970-01-01 for a proleptic Gregorian date.
 * Howard Hinnant's days_from_civil: shifts the year to start in March so the
 * leap day lands at the end of the era, removing every special case.
 */
static int32_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= (m <= 2) ? 1 : 0;

    const int32_t era = ((y >= 0) ? y : (y - 399)) / 400;
    const int32_t yoe = y - (era * 400);                       /* 0..399   */
    const int32_t doy = (((153 * (m + ((m > 2) ? -3 : 9))) + 2) / 5) + d - 1; /* 0..365 */
    const int32_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;          /* 0..146096 */

    return (era * 146097) + doe - 719468;
}

/** Inverse of days_from_civil. */
static void civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d)
{
    z += 719468;

    const int32_t era = ((z >= 0) ? z : (z - 146096)) / 146097;
    const int32_t doe = z - (era * 146097);                                  /* 0..146096 */
    const int32_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365; /* 0..399 */
    const int32_t yr  = yoe + (era * 400);
    const int32_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));       /* 0..365 */
    const int32_t mp  = ((5 * doy) + 2) / 153;                               /* 0..11  */
    const int32_t dy  = doy - (((153 * mp) + 2) / 5) + 1;                    /* 1..31  */
    const int32_t mn  = mp + ((mp < 10) ? 3 : -9);                           /* 1..12  */

    *y = yr + ((mn <= 2) ? 1 : 0);
    *m = mn;
    *d = dy;
}

app_epoch_t time_to_epoch(const app_datetime_t *dt)
{
    if (!time_is_valid(dt)) {
        return 0u;
    }

    int32_t days = days_from_civil((int32_t)dt->year, (int32_t)dt->month, (int32_t)dt->day)
                   - DAYS_1970_TO_2000;
    if (days < 0) {
        return 0u;
    }

    return ((uint32_t)days * SECS_PER_DAY)
         + ((uint32_t)dt->hour * SECS_PER_HOUR)
         + ((uint32_t)dt->minute * SECS_PER_MIN)
         + (uint32_t)dt->second;
}

void time_from_epoch(app_epoch_t epoch, app_datetime_t *out)
{
    if (out == NULL) {
        return;
    }

    uint32_t days = epoch / SECS_PER_DAY;
    uint32_t rem  = epoch % SECS_PER_DAY;

    int32_t y, m, d;
    civil_from_days((int32_t)days + DAYS_1970_TO_2000, &y, &m, &d);

    out->year   = (uint16_t)y;
    out->month  = (uint8_t)m;
    out->day    = (uint8_t)d;
    out->hour   = (uint8_t)(rem / SECS_PER_HOUR);
    out->minute = (uint8_t)((rem % SECS_PER_HOUR) / SECS_PER_MIN);
    out->second = (uint8_t)(rem % SECS_PER_MIN);
}
