/**
 * @file  rtc_time.h
 * @brief Wall-clock time, backed by the RTC with LSE.
 *
 * OCPP timestamps are ISO 8601 UTC and the CSMS uses them for billing, so the
 * clock has to be right. The RTC is set from BootNotification.conf and from
 * Heartbeat.conf; until one of those lands the clock is flagged unsynchronised
 * and transactions are journalled with a monotonic tick instead, then rewritten
 * with real timestamps once the offset is known.
 */
#ifndef RTC_TIME_H
#define RTC_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void rtc_time_init(void);

/** Seconds since the Unix epoch, UTC. Zero if never synchronised. */
uint32_t rtc_unix_time(void);

/** Set the clock from a Unix timestamp. */
void rtc_set_unix_time(uint32_t unix_seconds);

/**
 * Parse an ISO 8601 UTC timestamp ("2026-08-18T14:03:00.000Z") into Unix
 * seconds. Fractional seconds and a trailing Z are accepted and ignored.
 * @return 0 if the string could not be parsed.
 */
uint32_t rtc_parse_iso8601(const char *iso);

/**
 * Format Unix seconds as ISO 8601 UTC with millisecond precision, which is
 * what OCPP 1.6 expects.
 * @return number of characters written, excluding the NUL.
 */
size_t rtc_format_iso8601(uint32_t unix_seconds, char *out, size_t out_len);

/** True once the clock has been set from the CSMS. */
bool rtc_is_synced(void);

#endif /* RTC_TIME_H */
