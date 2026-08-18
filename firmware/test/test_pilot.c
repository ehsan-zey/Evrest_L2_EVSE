/**
 * Host-side unit tests for the pure control-pilot and proximity logic.
 * These are the functions where a wrong number means a car charges at the
 * wrong current or a fault state decodes as "ready", so they are worth
 * pinning down independently of any hardware.
 */
#include "cp_pilot.h"
#include "proximity.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do {                                    \
    if (!(cond)) { g_fail++;                                     \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);            \
        printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

#define CHECK_NEAR(a, b, tol, ...) do {                          \
    if (fabsf((float)(a) - (float)(b)) > (tol)) { g_fail++;      \
        printf("  FAIL %s:%d  got %.3f want %.3f  ",             \
               __FILE__, __LINE__, (double)(a), (double)(b));    \
        printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

static void test_state_decode(void)
{
    printf("cp_decode_state\n");
    /* Nominal plateau voltages must land in the right state. */
    CHECK(cp_decode_state( 12000) == CP_STATE_A, "12 V -> A");
    CHECK(cp_decode_state(  9000) == CP_STATE_B, "9 V -> B");
    CHECK(cp_decode_state(  6000) == CP_STATE_C, "6 V -> C");
    CHECK(cp_decode_state(  3000) == CP_STATE_D, "3 V -> D");
    CHECK(cp_decode_state(     0) == CP_STATE_E, "0 V -> E");
    CHECK(cp_decode_state(-12000) == CP_STATE_F, "-12 V -> F");

    /* IEC 61851-1 Table A.4 acceptance-window edges. */
    CHECK(cp_decode_state( 11400) == CP_STATE_A, "A window low edge");
    CHECK(cp_decode_state( 12600) == CP_STATE_A, "A window high edge");
    CHECK(cp_decode_state(  8360) == CP_STATE_B, "B window low edge");
    CHECK(cp_decode_state(  9560) == CP_STATE_B, "B window high edge");
    CHECK(cp_decode_state(  5480) == CP_STATE_C, "C window low edge");
    CHECK(cp_decode_state(  6530) == CP_STATE_C, "C window high edge");
    CHECK(cp_decode_state(  2620) == CP_STATE_D, "D window low edge");
    CHECK(cp_decode_state(  3250) == CP_STATE_D, "D window high edge");

    /* Out-of-range readings must be rejected, not clamped into a valid state. */
    CHECK(cp_decode_state( 14000) == CP_STATE_INVALID, "over-range positive");
    CHECK(cp_decode_state(-14000) == CP_STATE_INVALID, "over-range negative");

    /* Boundaries are exclusive downward: exactly on the split takes the
     * higher state, and one millivolt below takes the lower one. */
    CHECK(cp_decode_state(10500) == CP_STATE_A, "A/B boundary");
    CHECK(cp_decode_state(10499) == CP_STATE_B, "just below A/B boundary");
    CHECK(cp_decode_state( 7500) == CP_STATE_B, "B/C boundary");
    CHECK(cp_decode_state( 7499) == CP_STATE_C, "just below B/C boundary");
}

static void test_duty_current_roundtrip(void)
{
    printf("cp_duty_for_current / cp_current_for_duty\n");

    /* IEC 61851-1 Table A.7 anchor points, low branch: I = duty * 0.6 */
    CHECK_NEAR(cp_duty_for_current( 6.0f), 10.0f, 0.01f, "6 A -> 10 %%");
    CHECK_NEAR(cp_duty_for_current(16.0f), 26.667f, 0.01f, "16 A");
    CHECK_NEAR(cp_duty_for_current(32.0f), 53.333f, 0.01f, "32 A");
    CHECK_NEAR(cp_duty_for_current(48.0f), 80.0f, 0.01f, "48 A -> 80 %%");
    CHECK_NEAR(cp_duty_for_current(51.0f), 85.0f, 0.01f, "51 A -> 85 %%");

    /* High branch: I = (duty - 64) * 2.5 */
    CHECK_NEAR(cp_duty_for_current(63.0f), 89.2f, 0.01f, "63 A");
    CHECK_NEAR(cp_duty_for_current(80.0f), 96.0f, 0.01f, "80 A -> 96 %%");

    /* Above the table maximum the offer saturates rather than wrapping. */
    CHECK_NEAR(cp_duty_for_current(100.0f), 96.0f, 0.01f, "clamped to 80 A");

    /* Below the 6 A minimum there is no legal duty; the caller must go static. */
    CHECK(cp_duty_for_current(5.9f) == 0.0f, "below minimum -> no PWM");
    CHECK(cp_duty_for_current(0.0f) == 0.0f, "zero -> no PWM");

    /* Round-trip every value the DIP switches can select. */
    const float dip_currents[] = { 16.0f, 24.0f, 32.0f, 40.0f, 48.0f };
    for (size_t i = 0; i < sizeof(dip_currents)/sizeof(dip_currents[0]); i++) {
        float a = dip_currents[i];
        float back = cp_current_for_duty(cp_duty_for_current(a));
        CHECK_NEAR(back, a, 0.05f, "round-trip %.0f A", (double)a);
    }

    /* Reverse mapping at the table's defined points. */
    CHECK_NEAR(cp_current_for_duty(10.0f),  6.0f, 0.01f, "10 %% -> 6 A");
    CHECK_NEAR(cp_current_for_duty(50.0f), 30.0f, 0.01f, "50 %% -> 30 A");
    CHECK_NEAR(cp_current_for_duty(85.0f), 51.0f, 0.01f, "85 %% -> 51 A");
    CHECK_NEAR(cp_current_for_duty(90.0f), 65.0f, 0.01f, "90 %% -> 65 A");
    CHECK_NEAR(cp_current_for_duty(96.0f), 80.0f, 0.01f, "96 %% -> 80 A");

    /* The 8-10 %% band is a flat 6 A, not a proportional value. */
    CHECK_NEAR(cp_current_for_duty(8.0f), 6.0f, 0.01f, "8 %% -> 6 A flat");
    CHECK_NEAR(cp_current_for_duty(9.0f), 6.0f, 0.01f, "9 %% -> 6 A flat");

    /* Regions that encode no current must return 0, never a plausible value. */
    CHECK(cp_current_for_duty(2.0f)  == 0.0f, "below 3 %% is not allowed");
    CHECK(cp_current_for_duty(5.0f)  == 0.0f, "5 %% is digital comms, not 3 A");
    CHECK(cp_current_for_duty(7.5f)  == 0.0f, "7-8 %% is a forbidden gap");
    CHECK(cp_current_for_duty(99.0f) == 0.0f, ">97 %% is not an offer");

    CHECK(cp_duty_is_digital_comm(5.0f),  "5 %% is the digital-comms signal");
    CHECK(!cp_duty_is_digital_comm(10.0f), "10 %% is a current offer");
}

static void test_adc_calibration(void)
{
    printf("cp_adc_to_mv\n");
    /*
     * The calibration must map the ADC codes the hardware was characterised at
     * onto the states the original firmware measured: ~3100 counts = state A,
     * ~2100-3100 = B, ~1200-2099 = C, ~400-1199 = D.
     */
    CHECK(cp_decode_state(cp_adc_to_mv(3300)) == CP_STATE_A, "3300 counts -> A");
    CHECK(cp_decode_state(cp_adc_to_mv(2600)) == CP_STATE_B, "2600 counts -> B");
    CHECK(cp_decode_state(cp_adc_to_mv(1700)) == CP_STATE_C, "1700 counts -> C");
    CHECK(cp_decode_state(cp_adc_to_mv( 800)) == CP_STATE_D, "800 counts -> D");
    /* Monotonic: more counts must never mean less voltage. */
    for (uint16_t c = 1; c < 4095; c += 97) {
        CHECK(cp_adc_to_mv(c) > cp_adc_to_mv((uint16_t)(c - 1)), "monotonic at %u", c);
    }
}

static void test_median(void)
{
    printf("cp_median_u16\n");
    /* A single wild spike must not move the result. This is the whole point of
     * using a median rather than a mean for the plateau readings. */
    uint16_t spike[8] = { 2000, 2001, 1999, 2000, 4095, 2000, 2002, 1998 };
    uint16_t m = cp_median_u16(spike, 8);
    CHECK(m >= 1999 && m <= 2002, "spike rejected, got %u", m);

    uint16_t flat[5] = { 1234, 1234, 1234, 1234, 1234 };
    CHECK(cp_median_u16(flat, 5) == 1234, "constant input");

    uint16_t ramp[5] = { 5, 4, 3, 2, 1 };
    CHECK(cp_median_u16(ramp, 5) == 3, "reversed input sorts correctly");

    uint16_t one[1] = { 77 };
    CHECK(cp_median_u16(one, 1) == 77, "single sample");
}

static void test_proximity(void)
{
    printf("proximity decode\n");
    pp_status_t st;

    /* Type 2 cable coding resistors and the ratings they declare. */
    struct { uint32_t ohms; float amps; } t2[] = {
        { 1500u, 13.0f }, { 680u, 20.0f }, { 220u, 32.0f }, { 100u, 63.0f },
    };
    for (size_t i = 0; i < sizeof(t2)/sizeof(t2[0]); i++) {
        pp_decode(t2[i].ohms, CONNECTOR_TYPE2_IEC62196, &st);
        CHECK(st.cable_present, "%u R cable present", t2[i].ohms);
        CHECK_NEAR(st.cable_rating_a, t2[i].amps, 0.01f, "%u R rating", t2[i].ohms);
    }

    /* An open line means no cable, regardless of connector type. */
    pp_decode(UINT32_MAX, CONNECTOR_TYPE2_IEC62196, &st);
    CHECK(!st.cable_present, "open PP -> no cable");

    /* A present but unrecognised resistance must not report a rating. A
     * counterfeit cable falling back to 'maximum' is the dangerous direction. */
    pp_decode(390u, CONNECTOR_TYPE2_IEC62196, &st);
    CHECK(st.cable_present, "unknown code still counts as present");
    CHECK(st.cable_rating_a == 0.0f, "unknown code declares no rating");

    /* Type 1: latch state only, never a rating. */
    pp_decode(2700u, CONNECTOR_TYPE1_J1772, &st);
    CHECK(st.latch_engaged, "2700 R -> latch engaged");
    CHECK(st.cable_rating_a == 0.0f, "Type 1 carries no cable rating");
    pp_decode(480u, CONNECTOR_TYPE1_J1772, &st);
    CHECK(!st.latch_engaged, "480 R -> release pressed");

    printf("pp_adc_to_ohms\n");
    /* Rail means open. Mid-scale must invert the divider correctly:
     * v = 1650 mV is half supply, so Rpp == Rpullup. */
    CHECK(pp_adc_to_ohms(4095) == UINT32_MAX, "full scale -> open");
    CHECK_NEAR(pp_adc_to_ohms(2047), 330.0f, 2.0f, "half scale -> Rpullup");
}

int main(void)
{
    printf("=== EVSE pure-logic tests ===\n");
    test_state_decode();
    test_duty_current_roundtrip();
    test_adc_calibration();
    test_median();
    test_proximity();

    if (g_fail == 0) { printf("\nAll tests passed.\n"); return 0; }
    printf("\n%d test(s) FAILED.\n", g_fail);
    return 1;
}
