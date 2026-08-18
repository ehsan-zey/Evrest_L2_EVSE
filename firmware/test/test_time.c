/**
 * Host-side tests for ISO 8601 handling.
 * OCPP timestamps drive billing, so an off-by-one in the calendar maths turns
 * into a customer being charged for the wrong day.
 */
#include "rtc_time.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)

static void test_parse(void)
{
    printf("rtc_parse_iso8601\n");
    CHECK(rtc_parse_iso8601("1970-01-01T00:00:00Z") == 0u, "epoch");
    CHECK(rtc_parse_iso8601("2000-01-01T00:00:00Z") == 946684800u, "Y2K");
    CHECK(rtc_parse_iso8601("2026-08-18T14:03:00Z") == 1787061780u, "a real time");
    /* Fractional seconds are what OCPP servers actually send. */
    CHECK(rtc_parse_iso8601("2026-08-18T14:03:00.123Z") == 1787061780u, "fractional");

    /* Leap years: 2024 has a 29 February, 2100 does not. */
    CHECK(rtc_parse_iso8601("2024-02-29T00:00:00Z") == 1709164800u, "leap day");
    CHECK(rtc_parse_iso8601("2024-03-01T00:00:00Z") -
          rtc_parse_iso8601("2024-02-29T00:00:00Z") == 86400u, "day after leap day");
    CHECK(rtc_parse_iso8601("2023-03-01T00:00:00Z") -
          rtc_parse_iso8601("2023-02-28T00:00:00Z") == 86400u, "non-leap February");

    CHECK(rtc_parse_iso8601("garbage") == 0u, "garbage rejected");
    CHECK(rtc_parse_iso8601(NULL) == 0u, "NULL rejected");
    CHECK(rtc_parse_iso8601("2026-13-01T00:00:00Z") == 0u, "month 13 rejected");
    CHECK(rtc_parse_iso8601("2026-08-18T25:00:00Z") == 0u, "hour 25 rejected");
}

static void test_format_roundtrip(void)
{
    printf("rtc_format_iso8601 round trip\n");
    char buf[40];

    rtc_format_iso8601(1787061780u, buf, sizeof(buf));
    CHECK(strcmp(buf, "2026-08-18T14:03:00.000Z") == 0, "format, got %s", buf);

    /* Every value must survive format -> parse unchanged. Step through several
     * years including leap-day boundaries. */
    uint32_t samples[] = {
        0u, 946684800u, 1709164800u, 1709251200u,
        1787061780u, 2000000000u
    };
    (void)samples;
    for (uint32_t t = 0; t < 2100000000u; t += 7919999u) {
        rtc_format_iso8601(t, buf, sizeof(buf));
        uint32_t back = rtc_parse_iso8601(buf);
        CHECK(back == t, "round trip %lu -> %s -> %lu",
              (unsigned long)t, buf, (unsigned long)back);
        if (back != t) break;
    }
}

int main(void)
{
    printf("=== time tests ===\n");
    test_parse();
    test_format_roundtrip();
    if (g_fail == 0) { printf("\nAll tests passed.\n"); return 0; }
    printf("\n%d test(s) FAILED.\n", g_fail);
    return 1;
}
