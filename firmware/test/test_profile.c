/**
 * Host-side tests for the SmartCharging composite schedule.
 *
 * Getting this wrong is invisible until a site's main breaker trips, so the
 * precedence rules from OCPP 1.6 section 3.13 are pinned down explicitly.
 */
#include "ocpp_profile.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)
#define CHECK_NEAR(a, b, tol, ...) do {                         \
    if (fabsf((float)(a)-(float)(b)) > (tol)) { g_fail++;       \
        printf("  FAIL %s:%d  got %.2f want %.2f  ",            \
               __FILE__, __LINE__, (double)(a), (double)(b));   \
        printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)

#define T0  1787000000u    /* an arbitrary "now" */

static ocpp_profile_t mk(int32_t id, ocpp_purpose_t purpose, int32_t stack,
                         ocpp_kind_t kind, uint32_t start, int32_t duration)
{
    ocpp_profile_t p;
    memset(&p, 0, sizeof(p));
    p.valid = true;
    p.profile_id = id;
    p.transaction_id = -1;
    p.purpose = purpose;
    p.stack_level = stack;
    p.kind = kind;
    p.rate_unit = OCPP_RATE_AMPS;
    p.start_schedule = start;
    p.duration_s = duration;
    return p;
}

static void add_period(ocpp_profile_t *p, int32_t start, float limit)
{
    p->periods[p->period_count].start_period = start;
    p->periods[p->period_count].limit = limit;
    p->period_count++;
}

static void test_single_absolute(void)
{
    printf("single absolute profile\n");
    ocpp_profile_init();

    ocpp_profile_t p = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&p, 0,    32.0f);
    add_period(&p, 3600, 16.0f);
    add_period(&p, 7200,  8.0f);
    CHECK(ocpp_profile_set(&p), "profile stored");

    CHECK_NEAR(ocpp_profile_limit_a(T0,        0, 240, 1, 48), 32.0f, 0.01f, "t=0");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 1800, 0, 240, 1, 48), 32.0f, 0.01f, "t=30min");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 3600, 0, 240, 1, 48), 16.0f, 0.01f, "t=1h exactly");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 5000, 0, 240, 1, 48), 16.0f, 0.01f, "t=83min");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 9000, 0, 240, 1, 48),  8.0f, 0.01f, "t=2.5h");

    /* Before the schedule starts, the fallback applies rather than period 0. */
    CHECK_NEAR(ocpp_profile_limit_a(T0 - 10, 0, 240, 1, 48), 48.0f, 0.01f, "before start");
}

static void test_duration_expiry(void)
{
    printf("duration ends the schedule\n");
    ocpp_profile_init();

    ocpp_profile_t p = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 3600);
    add_period(&p, 0, 10.0f);
    ocpp_profile_set(&p);

    CHECK_NEAR(ocpp_profile_limit_a(T0 + 3599, 0, 240, 1, 48), 10.0f, 0.01f, "inside");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 3600, 0, 240, 1, 48), 48.0f, 0.01f, "at the end");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 9999, 0, 240, 1, 48), 48.0f, 0.01f, "past the end");
}

static void test_purpose_precedence(void)
{
    printf("TxProfile beats TxDefaultProfile\n");
    ocpp_profile_init();

    /* TxDefault at a HIGH stack level, TxProfile at a LOW one. Purpose must
     * still win — this is the case a stack-level-only comparison gets wrong. */
    ocpp_profile_t def = mk(1, OCPP_PURPOSE_TX_DEFAULT, 9, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&def, 0, 32.0f);
    ocpp_profile_set(&def);

    ocpp_profile_t tx = mk(2, OCPP_PURPOSE_TX, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&tx, 0, 10.0f);
    ocpp_profile_set(&tx);

    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, T0, 240, 1, 48), 10.0f, 0.01f,
               "TxProfile wins with a transaction running");

    /* With no transaction, the TxProfile does not apply at all. */
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 32.0f, 0.01f,
               "TxDefault applies with no transaction");
}

static void test_stack_level(void)
{
    printf("higher stack level wins within a purpose\n");
    ocpp_profile_init();

    ocpp_profile_t low = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&low, 0, 32.0f);
    ocpp_profile_set(&low);

    ocpp_profile_t high = mk(2, OCPP_PURPOSE_TX_DEFAULT, 5, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&high, 0, 20.0f);
    ocpp_profile_set(&high);

    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 20.0f, 0.01f,
               "stack 5 wins");

    /* Levels do not blend: a higher stack level with a HIGHER limit also wins,
     * even though that raises the limit. */
    ocpp_profile_t higher = mk(3, OCPP_PURPOSE_TX_DEFAULT, 9, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&higher, 0, 40.0f);
    ocpp_profile_set(&higher);
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 40.0f, 0.01f,
               "highest stack wins even when less restrictive");
}

static void test_charge_point_max_caps(void)
{
    printf("ChargePointMaxProfile caps but never raises\n");
    ocpp_profile_init();

    ocpp_profile_t tx = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&tx, 0, 32.0f);
    ocpp_profile_set(&tx);

    ocpp_profile_t cap = mk(2, OCPP_PURPOSE_CHARGE_POINT_MAX, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&cap, 0, 16.0f);
    ocpp_profile_set(&cap);
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 16.0f, 0.01f, "cap applies");

    /* A cap above the Tx limit must not raise it. */
    ocpp_profile_clear(2, OCPP_PURPOSE_INVALID, -1);
    ocpp_profile_t cap2 = mk(3, OCPP_PURPOSE_CHARGE_POINT_MAX, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&cap2, 0, 40.0f);
    ocpp_profile_set(&cap2);
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 32.0f, 0.01f,
               "high cap does not raise the Tx limit");

    /* A cap with no other profile still applies against the fallback. */
    ocpp_profile_clear(1, OCPP_PURPOSE_INVALID, -1);
    ocpp_profile_clear(3, OCPP_PURPOSE_INVALID, -1);
    ocpp_profile_t cap3 = mk(4, OCPP_PURPOSE_CHARGE_POINT_MAX, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&cap3, 0, 12.0f);
    ocpp_profile_set(&cap3);
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 12.0f, 0.01f,
               "cap applies to the fallback");
}

static void test_relative(void)
{
    printf("relative profiles anchor on the transaction\n");
    ocpp_profile_init();

    ocpp_profile_t p = mk(1, OCPP_PURPOSE_TX, 0, OCPP_KIND_RELATIVE, 0, 0);
    add_period(&p, 0,   6.0f);     /* gentle ramp-in */
    add_period(&p, 300, 32.0f);
    ocpp_profile_set(&p);

    uint32_t txn = T0;
    CHECK_NEAR(ocpp_profile_limit_a(txn + 10,  txn, 240, 1, 48),  6.0f, 0.01f, "first 5 min");
    CHECK_NEAR(ocpp_profile_limit_a(txn + 400, txn, 240, 1, 48), 32.0f, 0.01f, "after 5 min");

    /* With no transaction there is no anchor, so it must not apply. */
    CHECK_NEAR(ocpp_profile_limit_a(txn + 400, 0, 240, 1, 48), 48.0f, 0.01f,
               "no transaction, no relative profile");
}

static void test_recurring_daily(void)
{
    printf("recurring daily schedule\n");
    ocpp_profile_init();

    /* Anchor at midnight: cheap rate overnight, restricted during the day. */
    uint32_t midnight = 1786924800u;   /* 2026-08-18T00:00:00Z */
    ocpp_profile_t p = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_RECURRING,
                          midnight, 0);
    p.recurrency = OCPP_RECUR_DAILY;
    add_period(&p, 0,      32.0f);   /* 00:00 */
    add_period(&p, 25200,  10.0f);   /* 07:00 */
    add_period(&p, 72000,  32.0f);   /* 20:00 */
    ocpp_profile_set(&p);

    CHECK_NEAR(ocpp_profile_limit_a(midnight + 3600,  0, 240, 1, 48), 32.0f, 0.01f, "01:00");
    CHECK_NEAR(ocpp_profile_limit_a(midnight + 36000, 0, 240, 1, 48), 10.0f, 0.01f, "10:00");
    CHECK_NEAR(ocpp_profile_limit_a(midnight + 75600, 0, 240, 1, 48), 32.0f, 0.01f, "21:00");

    /* Three days later the same pattern must repeat. */
    uint32_t d3 = midnight + 3u * 86400u;
    CHECK_NEAR(ocpp_profile_limit_a(d3 + 36000, 0, 240, 1, 48), 10.0f, 0.01f,
               "10:00 three days later");

    /* And it must work for a "now" BEFORE the anchor, which is what happens
     * when the CSMS anchors a recurring profile at a future midnight. */
    CHECK_NEAR(ocpp_profile_limit_a(midnight - 86400u + 36000u, 0, 240, 1, 48),
               10.0f, 0.01f, "10:00 the day before the anchor");
}

static void test_watt_conversion(void)
{
    printf("watt-based profiles convert to amps\n");
    ocpp_profile_init();

    ocpp_profile_t p = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    p.rate_unit = OCPP_RATE_WATTS;
    add_period(&p, 0, 7680.0f);       /* 7.68 kW at 240 V single phase = 32 A */
    ocpp_profile_set(&p);

    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240.0f, 1, 48), 32.0f, 0.01f,
               "7680 W / 240 V = 32 A");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240.0f, 3, 48), 10.667f, 0.01f,
               "same power over three phases");
}

static void test_replace_and_clear(void)
{
    printf("set replaces, clear filters\n");
    ocpp_profile_init();

    ocpp_profile_t a = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&a, 0, 32.0f);
    ocpp_profile_set(&a);
    CHECK(ocpp_profile_count() == 1, "one profile");

    /* Same id -> replace, not add. */
    ocpp_profile_t b = mk(1, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&b, 0, 16.0f);
    ocpp_profile_set(&b);
    CHECK(ocpp_profile_count() == 1, "still one profile after same-id set");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 16.0f, 0.01f, "replaced");

    /* Different id, same purpose and stack level -> also replace. */
    ocpp_profile_t c = mk(7, OCPP_PURPOSE_TX_DEFAULT, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&c, 0, 24.0f);
    ocpp_profile_set(&c);
    CHECK(ocpp_profile_count() == 1, "same purpose+stack replaces");
    CHECK_NEAR(ocpp_profile_limit_a(T0 + 60, 0, 240, 1, 48), 24.0f, 0.01f, "replaced again");

    /* Clear by purpose. */
    ocpp_profile_t tx = mk(9, OCPP_PURPOSE_TX, 0, OCPP_KIND_ABSOLUTE, T0, 0);
    add_period(&tx, 0, 8.0f);
    ocpp_profile_set(&tx);
    CHECK(ocpp_profile_count() == 2, "two profiles");
    CHECK(ocpp_profile_clear(-1, OCPP_PURPOSE_TX, -1) == 1, "cleared one by purpose");
    CHECK(ocpp_profile_count() == 1, "one left");

    /* Clear everything. */
    CHECK(ocpp_profile_clear(-1, OCPP_PURPOSE_INVALID, -1) == 1, "cleared the rest");
    CHECK(ocpp_profile_count() == 0, "empty");
    CHECK_NEAR(ocpp_profile_limit_a(T0, 0, 240, 1, 48), 48.0f, 0.01f, "fallback returns");
}

static void test_capacity(void)
{
    printf("storage capacity is bounded\n");
    ocpp_profile_init();
    /* Distinct purpose/stack combinations so none of them replace each other. */
    int stored = 0;
    for (int i = 0; i < OCPP_MAX_PROFILES + 4; i++) {
        ocpp_profile_t p = mk(100 + i, OCPP_PURPOSE_TX_DEFAULT, i,
                              OCPP_KIND_ABSOLUTE, T0, 0);
        add_period(&p, 0, 10.0f + (float)i);
        if (ocpp_profile_set(&p)) stored++;
    }
    CHECK(stored == OCPP_MAX_PROFILES, "stored %d, cap is %d",
          stored, OCPP_MAX_PROFILES);
    CHECK(ocpp_profile_count() == OCPP_MAX_PROFILES, "count matches");
}

int main(void)
{
    printf("=== SmartCharging profile tests ===\n");
    test_single_absolute();
    test_duration_expiry();
    test_purpose_precedence();
    test_stack_level();
    test_charge_point_max_caps();
    test_relative();
    test_recurring_daily();
    test_watt_conversion();
    test_replace_and_clear();
    test_capacity();
    if (g_fail == 0) { printf("\nAll tests passed.\n"); return 0; }
    printf("\n%d test(s) FAILED.\n", g_fail);
    return 1;
}
