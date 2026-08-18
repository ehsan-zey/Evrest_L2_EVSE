/**
 * @file  proximity.c
 * @brief Proximity pilot decoding. See proximity.h.
 */
#include "proximity.h"
#include "cp_pilot.h"
#include "evse_board.h"
#include <stdlib.h>

/**
 * Type 2 coding resistors and the ratings they declare, per IEC 62196-2.
 *
 * Bands allow for the cable resistor tolerance, the pull-up tolerance and ADC
 * error — roughly +/-50 % around nominal — but deliberately leave GAPS between
 * them. A resistance landing in a gap is reported as "present, rating unknown"
 * and falls back to the 6 A minimum. Contiguous bands would instead round a
 * damaged or counterfeit cable up to the next rating, which is the one
 * direction this must not fail in.
 */
typedef struct { uint32_t nominal; uint32_t lo; uint32_t hi; float amps; } pp_code_t;

static const pp_code_t PP_TYPE2_CODES[] = {
    { 1500u, 1100u, 2200u, 13.0f },   /* gap 1001..1099 */
    {  680u,  500u, 1000u, 20.0f },   /* gap  331..499  */
    {  220u,  160u,  330u, 32.0f },   /* gap  151..159  */
    {  100u,   75u,  150u, 63.0f },
};

/* Type 1 latch network. */
#define PP_T1_LATCH_CLOSED_LO   1500u
#define PP_T1_LATCH_CLOSED_HI   4000u
#define PP_T1_LATCH_OPEN_LO      250u
#define PP_T1_LATCH_OPEN_HI     1000u

/** Above this the PP line is open — no cable in the socket. */
#define PP_OPEN_THRESHOLD_OHMS  10000u

uint32_t pp_adc_to_ohms(uint16_t adc_raw)
{
    /*
     * Divider: 3V3 -- Rpullup -- node(ADC) -- Rpp -- PE
     *   v = 3V3 * Rpp / (Rpullup + Rpp)   =>   Rpp = Rpullup * v / (3V3 - v)
     */
    float v_mv = ((float)adc_raw * PP_SUPPLY_MV) / 4095.0f;
    if (v_mv >= PP_SUPPLY_MV - 1.0f) {
        return UINT32_MAX;                    /* rail: PP open, no cable */
    }
    if (v_mv <= 0.0f) {
        return 0u;                            /* shorted to PE           */
    }
    float r = PP_PULLUP_OHMS * v_mv / (PP_SUPPLY_MV - v_mv);
    if (r >= (float)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(r + 0.5f);
}

void pp_decode(uint32_t ohms, connector_type_t type, pp_status_t *out)
{
    if (out == NULL) return;

    out->resistance_ohms = ohms;
    out->cable_rating_a  = 0.0f;
    out->cable_present   = false;
    out->latch_engaged   = false;

    if (type == CONNECTOR_TETHERED) {
        /* Captive cable: always "present", rating comes from the SKU. */
        out->cable_present = true;
        out->latch_engaged = true;
        return;
    }

    if (ohms >= PP_OPEN_THRESHOLD_OHMS) {
        return;                                /* nothing plugged in */
    }

    if (type == CONNECTOR_TYPE2_IEC62196) {
        for (size_t i = 0; i < sizeof(PP_TYPE2_CODES)/sizeof(PP_TYPE2_CODES[0]); i++) {
            if (ohms >= PP_TYPE2_CODES[i].lo && ohms <= PP_TYPE2_CODES[i].hi) {
                out->cable_present  = true;
                out->latch_engaged  = true;   /* Type 2 has no latch signal */
                out->cable_rating_a = PP_TYPE2_CODES[i].amps;
                return;
            }
        }
        /*
         * A resistance that is present but matches no code means a damaged or
         * counterfeit cable. Report it present with no rating; the limit
         * calculation then falls back to the 6 A minimum rather than the SKU
         * maximum, which is the safe direction to be wrong in.
         */
        out->cable_present  = true;
        out->cable_rating_a = 0.0f;
        return;
    }

    /* Type 1 (J1772): latch button state only. */
    out->cable_present = true;
    if (ohms >= PP_T1_LATCH_CLOSED_LO && ohms <= PP_T1_LATCH_CLOSED_HI) {
        out->latch_engaged = true;
    } else if (ohms >= PP_T1_LATCH_OPEN_LO && ohms <= PP_T1_LATCH_OPEN_HI) {
        out->latch_engaged = false;           /* release pressed */
    } else {
        out->latch_engaged = false;           /* unrecognised: assume unsafe */
    }
}

#ifndef EVSE_HOST_TEST

static connector_type_t s_type;
static pp_status_t      s_status;

void proximity_init(connector_type_t type)
{
    s_type = type;
    pp_decode(UINT32_MAX, type, &s_status);
}

void proximity_update(void)
{
    uint32_t ohms = pp_adc_to_ohms(cp_pilot_pp_raw());
    pp_decode(ohms, s_type, &s_status);
}

void proximity_get(pp_status_t *out)
{
    if (out) *out = s_status;
}

float proximity_current_limit(void)
{
    if (s_type != CONNECTOR_TYPE2_IEC62196) {
        return EVSE_MAX_CURRENT_A;            /* PP carries no rating here */
    }
    if (!s_status.cable_present) {
        return EVSE_MAX_CURRENT_A;            /* nothing to constrain yet  */
    }
    if (s_status.cable_rating_a <= 0.0f) {
        return EVSE_MIN_CURRENT_A;            /* unreadable code: minimum  */
    }
    return s_status.cable_rating_a;
}

#endif /* !EVSE_HOST_TEST */
