/**
 * @file  rtc_time.c
 * @brief Wall-clock time. See rtc_time.h.
 */
#include "rtc_time.h"
#include <stdio.h>
#include <string.h>

#ifndef EVSE_HOST_TEST
#include "stm32h5xx_hal.h"
extern RTC_HandleTypeDef hrtc;
#endif

static bool s_synced;

/* ---------------------------------------------------------------------- */
/* Calendar maths (pure, unit-tested)                                     */
/* ---------------------------------------------------------------------- */

static bool is_leap(uint32_t y)
{
    return (y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u);
}

static const uint8_t DAYS_IN_MONTH[12] =
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/** Days since 1970-01-01 for a Gregorian date. */
static int32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
    int32_t days = 0;
    for (uint32_t yy = 1970u; yy < y; yy++) {
        days += is_leap(yy) ? 366 : 365;
    }
    for (uint32_t mm = 1u; mm < m; mm++) {
        days += DAYS_IN_MONTH[mm - 1u];
        if (mm == 2u && is_leap(y)) days += 1;
    }
    return days + (int32_t)d - 1;
}

/** Inverse of days_from_civil(). */
static void civil_from_days(int32_t days, uint32_t *y, uint32_t *m, uint32_t *d)
{
    uint32_t year = 1970u;
    while (1) {
        int32_t len = is_leap(year) ? 366 : 365;
        if (days < len) break;
        days -= len;
        year++;
    }
    uint32_t month = 1u;
    while (1) {
        int32_t len = DAYS_IN_MONTH[month - 1u];
        if (month == 2u && is_leap(year)) len += 1;
        if (days < len) break;
        days -= len;
        month++;
    }
    *y = year;
    *m = month;
    *d = (uint32_t)days + 1u;
}

uint32_t rtc_parse_iso8601(const char *iso)
{
    if (iso == NULL) return 0u;

    unsigned y, mo, d, h, mi, s;
    /*
     * Accept both "2026-08-18T14:03:00Z" and the fractional form OCPP servers
     * usually send. sscanf stops at the seconds field either way; anything
     * after it (".123Z", "+00:00") is ignored, which is correct only because
     * OCPP 1.6 requires UTC.
     */
    if (sscanf(iso, "%4u-%2u-%2uT%2u:%2u:%2u", &y, &mo, &d, &h, &mi, &s) != 6) {
        return 0u;
    }
    if (y < 1970u || mo < 1u || mo > 12u || d < 1u || d > 31u ||
        h > 23u || mi > 59u || s > 60u) {
        return 0u;
    }

    int32_t days = days_from_civil(y, mo, d);
    if (days < 0) return 0u;
    return (uint32_t)days * 86400u + h * 3600u + mi * 60u + s;
}

size_t rtc_format_iso8601(uint32_t unix_seconds, char *out, size_t out_len)
{
    uint32_t y, mo, d;
    civil_from_days((int32_t)(unix_seconds / 86400u), &y, &mo, &d);
    uint32_t rem = unix_seconds % 86400u;

    int n = snprintf(out, out_len, "%04lu-%02lu-%02luT%02lu:%02lu:%02lu.000Z",
                     (unsigned long)y, (unsigned long)mo, (unsigned long)d,
                     (unsigned long)(rem / 3600u),
                     (unsigned long)((rem % 3600u) / 60u),
                     (unsigned long)(rem % 60u));
    return (n < 0) ? 0u : (size_t)n;
}

bool rtc_is_synced(void) { return s_synced; }

/* ---------------------------------------------------------------------- */
/* RTC hardware                                                           */
/* ---------------------------------------------------------------------- */

#ifndef EVSE_HOST_TEST

/** Backup register used to remember that the clock was set. */
#define RTC_SYNC_MARKER_REG   RTC_BKP_DR0
#define RTC_SYNC_MARKER_VALUE 0x45565354UL   /* "EVST" */

void rtc_time_init(void)
{
    /*
     * VBAT keeps the RTC running across a power cycle, so a clock set on a
     * previous run is still valid. The marker distinguishes that from a cold
     * battery-less start where the calendar reads a default date.
     */
    s_synced = (HAL_RTCEx_BKUPRead(&hrtc, RTC_SYNC_MARKER_REG) == RTC_SYNC_MARKER_VALUE);
}

uint32_t rtc_unix_time(void)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef dt;

    /* HAL requires time to be read before date, or the shadow registers lock. */
    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return 0u;
    if (HAL_RTC_GetDate(&hrtc, &dt, RTC_FORMAT_BIN) != HAL_OK) return 0u;

    int32_t days = days_from_civil(2000u + dt.Year, dt.Month, dt.Date);
    if (days < 0) return 0u;
    return (uint32_t)days * 86400u + t.Hours * 3600u + t.Minutes * 60u + t.Seconds;
}

void rtc_set_unix_time(uint32_t unix_seconds)
{
    uint32_t y, mo, d;
    civil_from_days((int32_t)(unix_seconds / 86400u), &y, &mo, &d);
    uint32_t rem = unix_seconds % 86400u;

    if (y < 2000u || y > 2099u) return;   /* RTC year register is 00..99 */

    RTC_TimeTypeDef t = {0};
    t.Hours   = (uint8_t)(rem / 3600u);
    t.Minutes = (uint8_t)((rem % 3600u) / 60u);
    t.Seconds = (uint8_t)(rem % 60u);
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    RTC_DateTypeDef dt = {0};
    dt.Year  = (uint8_t)(y - 2000u);
    dt.Month = (uint8_t)mo;
    dt.Date  = (uint8_t)d;
    dt.WeekDay = RTC_WEEKDAY_MONDAY;   /* not used; RTC recomputes nothing */

    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return;
    if (HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN) != HAL_OK) return;

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_SYNC_MARKER_REG, RTC_SYNC_MARKER_VALUE);
    s_synced = true;
}

#else  /* host tests: a settable software clock */

static uint32_t s_fake_now;
void     rtc_time_init(void)                 { s_fake_now = 0; s_synced = false; }
uint32_t rtc_unix_time(void)                 { return s_fake_now; }
void     rtc_set_unix_time(uint32_t seconds) { s_fake_now = seconds; s_synced = true; }

#endif
