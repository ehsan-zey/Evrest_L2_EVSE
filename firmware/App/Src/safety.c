/**
 * @file  safety.c
 * @brief Protection supervisor implementation. See safety.h.
 */
#include "safety.h"
#include "relay.h"
#include "evse_board.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tuning                                                             */
/* ------------------------------------------------------------------ */

/** Consecutive 5 ms samples an input must hold before we believe it. */
#define DEBOUNCE_SAMPLES        3u

/**
 * Over-current policy. IEC 61851-1 requires the EVSE to stop supply if the
 * vehicle draws more than it was offered. Two thresholds, so a brief inrush
 * does not nuisance-trip but a genuine overdraw is caught quickly:
 *   >130 % of the offer for 100 ms, or >110 % for 5 s.
 */
#define OC_HARD_RATIO           1.30f
#define OC_HARD_MS              100u
#define OC_SOFT_RATIO           1.10f
#define OC_SOFT_MS              5000u
/** Below this the ratio test is meaningless (measurement noise dominates). */
#define OC_MIN_MEANINGFUL_A     2.0f

#define VOLTAGE_MIN_V           180.0f
#define VOLTAGE_MAX_V           270.0f
/** Mains excursions must persist this long — brownouts and sags are common. */
#define VOLTAGE_FAULT_MS        2000u

#if EVSE_TEMP_SENSORS_FITTED
/** NTC: 10 k at 25 C, B = 3950, 10 k pull-up to 3V3, NTC to ground. */
#define NTC_R_PULLUP            10000.0f
#define NTC_R_NOMINAL           10000.0f
#define NTC_T_NOMINAL_K         298.15f
#define NTC_BETA                3950.0f
#endif

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static volatile uint32_t s_faults;
static evse_earthing_t   s_earthing;

static uint8_t  s_pe_count, s_pen_count, s_weld_count;
static uint32_t s_oc_hard_since, s_oc_soft_since, s_uv_since, s_ov_since;
static int16_t  s_temp_conn_c = 25, s_temp_int_c = 25;
static volatile bool s_rcd_tripped;      /* set by the EXTI, cleared on reset */

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

static inline void fault_set(uint32_t bits)   { s_faults |= bits; }
static inline void fault_clr(uint32_t bits)   { s_faults &= ~bits; }

/**
 * Raise a fault and, if it is in the trip mask, drop the contactor now rather
 * than waiting for the state machine to notice.
 */
static void fault_raise(uint32_t bits)
{
    if ((s_faults & bits) == bits) {
        return;                       /* already known, nothing new to do */
    }
    fault_set(bits);
    if (bits & EVSE_FAULT_TRIP_MASK) {
        relay_emergency_open();
    }
    if (bits & EVSE_FAULT_LATCHING_MASK) {
        relay_inhibit();
    }
}

/* ------------------------------------------------------------------ */
/* Interrupt entry points                                             */
/* ------------------------------------------------------------------ */

void safety_rcd_isr(void)
{
    /*
     * Contactor first, bookkeeping second. Everything here must be safe at
     * interrupt priority: two GPIO writes and two stores.
     */
    relay_emergency_open();
    s_rcd_tripped = true;
    s_faults |= EVSE_FAULT_RCD_TRIP;
}

void safety_estop_isr(void)
{
    relay_emergency_open();
    s_faults |= EVSE_FAULT_ESTOP;
}

/* ------------------------------------------------------------------ */
/* Input helpers                                                      */
/* ------------------------------------------------------------------ */

static inline bool pe_present(void) {
    return HAL_GPIO_ReadPin(PE_DETECT_GPIO_PORT, PE_DETECT_GPIO_PIN) == PE_DETECT_OK_STATE;
}
static inline bool pen_fault_active(void) {
    return HAL_GPIO_ReadPin(PEN_FAULT_GPIO_PORT, PEN_FAULT_GPIO_PIN) == PEN_FAULT_ACTIVE_STATE;
}
static inline bool estop_active(void) {
    return HAL_GPIO_ReadPin(ESTOP_GPIO_PORT, ESTOP_GPIO_PIN) == ESTOP_ACTIVE_STATE;
}
static inline bool rcd_int_active(void) {
    return HAL_GPIO_ReadPin(RCD_INT_GPIO_PORT, RCD_INT_GPIO_PIN) == RCD_INT_ACTIVE_STATE;
}
static inline bool weld_detect_active(void) {
    return HAL_GPIO_ReadPin(WELD_DETECT_GPIO_PORT, WELD_DETECT_GPIO_PIN) == WELD_DETECT_ACTIVE_STATE;
}

/** True once `count` reaches the debounce depth; caller owns the counter. */
static bool debounce(bool condition, uint8_t *count)
{
    if (condition) {
        if (*count < DEBOUNCE_SAMPLES) (*count)++;
    } else {
        *count = 0;
    }
    return (*count >= DEBOUNCE_SAMPLES);
}

/**
 * Track how long a condition has been continuously true.
 * @param since  storage for the start tick; 0 means "not currently true"
 * @return true once the condition has held for at least @p ms
 */
static bool sustained(bool condition, uint32_t *since, uint32_t ms)
{
    uint32_t t = now_ms();
    if (!condition) { *since = 0; return false; }
    if (*since == 0) { *since = t ? t : 1u; return false; }
    return (uint32_t)(t - *since) >= ms;
}

/* ------------------------------------------------------------------ */
/* Temperature                                                        */
/* ------------------------------------------------------------------ */

/*
 * This board revision has no temperature sensor in the pinout, so the thermal
 * path is compiled out rather than faked. Connector-temperature monitoring is
 * required by UL 2594 and IEC 62196-1 for a Level-2 unit, so this must be
 * populated before certification — see EVSE_TEMP_SENSORS_FITTED in
 * evse_board.h for the free ADC pins.
 *
 * With no sensor, safety_thermal_derate() returns 1.0 and no over-temperature
 * fault can ever be raised. That is a known, deliberate gap, not an oversight.
 */

#if EVSE_TEMP_SENSORS_FITTED

extern ADC_HandleTypeDef hadc2;   /* aux ADC for the NTCs */

static uint16_t adc_read_blocking(uint32_t channel)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel      = channel;
    cfg.Rank         = ADC_REGULAR_RANK_1;
    cfg.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;   /* high source impedance */
    cfg.SingleDiff   = ADC_SINGLE_ENDED;
    cfg.OffsetNumber = ADC_OFFSET_NONE;
    cfg.Offset       = 0;

    if (HAL_ADC_ConfigChannel(&hadc2, &cfg) != HAL_OK)   return 0xFFFFu;
    if (HAL_ADC_Start(&hadc2) != HAL_OK)                 return 0xFFFFu;
    if (HAL_ADC_PollForConversion(&hadc2, 10) != HAL_OK) {
        HAL_ADC_Stop(&hadc2);
        return 0xFFFFu;
    }
    uint16_t v = (uint16_t)HAL_ADC_GetValue(&hadc2);
    HAL_ADC_Stop(&hadc2);
    return v;
}

/** Beta-model NTC conversion. Returns degrees Celsius. */
static int16_t ntc_to_celsius(uint16_t adc_raw)
{
    if (adc_raw == 0u || adc_raw >= 4095u) {
        return -127;                       /* open or shorted sensor */
    }
    /* Divider: 3V3 -- Rpullup -- node(ADC) -- NTC -- GND */
    float ratio  = (float)adc_raw / 4095.0f;
    float r_ntc  = NTC_R_PULLUP * ratio / (1.0f - ratio);
    float inv_t  = 1.0f / NTC_T_NOMINAL_K + logf(r_ntc / NTC_R_NOMINAL) / NTC_BETA;
    return (int16_t)((1.0f / inv_t) - 273.15f);
}

static void update_temperatures(void)
{
    uint16_t a = adc_read_blocking(TEMP_CONNECTOR_ADC_CHANNEL);
    uint16_t b = adc_read_blocking(TEMP_INTERNAL_ADC_CHANNEL);
    if (a != 0xFFFFu) s_temp_conn_c = ntc_to_celsius(a);
    if (b != 0xFFFFu) s_temp_int_c  = ntc_to_celsius(b);
}

float safety_thermal_derate(void)
{
    int16_t t = (s_temp_conn_c > s_temp_int_c) ? s_temp_conn_c : s_temp_int_c;

    /*
     * A disconnected sensor reads -127. Treating that as "cold" would disable
     * thermal protection silently, so fall back to a conservative half rate.
     */
    if (t <= -100)               return 0.5f;
    if (t < TEMP_DERATE_START_C) return 1.0f;
    if (t >= TEMP_TRIP_C)        return 0.0f;
    if (t >= TEMP_DERATE_FULL_C) return 0.5f;

    /* Linear roll-off from 100 % at DERATE_START to 50 % at DERATE_FULL. */
    float span = (float)(TEMP_DERATE_FULL_C - TEMP_DERATE_START_C);
    float into = (float)(t - TEMP_DERATE_START_C);
    return 1.0f - 0.5f * (into / span);
}

#else  /* !EVSE_TEMP_SENSORS_FITTED */

static void update_temperatures(void) { /* no sensor on this revision */ }
float safety_thermal_derate(void)      { return 1.0f; }

#endif

void safety_get_temps(int16_t *connector_c, int16_t *internal_c)
{
    if (connector_c) *connector_c = s_temp_conn_c;
    if (internal_c)  *internal_c  = s_temp_int_c;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void safety_init(evse_earthing_t earthing)
{
    s_earthing = earthing;
    s_faults = EVSE_FAULT_NONE;
    s_pe_count = s_pen_count = s_weld_count = 0;
    s_oc_hard_since = s_oc_soft_since = s_uv_since = s_ov_since = 0;
    s_rcd_tripped = false;

    HAL_GPIO_WritePin(RCD_TEST_GPIO_PORT, RCD_TEST_GPIO_PIN,
                      (RCD_TEST_ACTIVE_STATE == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

uint32_t safety_get_faults(void) { return s_faults; }

bool safety_is_latched(void) { return (s_faults & EVSE_FAULT_LATCHING_MASK) != 0u; }

void safety_update(const meter_reading_t *meter, float offered_a, bool charging)
{
    /* --- Emergency off: no debounce, this one is meant to be instant ---- */
    if (estop_active()) {
        fault_raise(EVSE_FAULT_ESTOP);
    } else {
        fault_clr(EVSE_FAULT_ESTOP);
    }

    /* --- RCD: the EXTI does the tripping; here we only track the level --- */
    if (rcd_int_active()) {
        fault_raise(EVSE_FAULT_RCD_TRIP);
    }

    /* --- Protective earth ---------------------------------------------- */
    if (debounce(!pe_present(), &s_pe_count)) {
        fault_raise(EVSE_FAULT_PE_LOST);
    } else if (s_pe_count == 0) {
        fault_clr(EVSE_FAULT_PE_LOST);
    }

    /* --- Open PEN, only meaningful on a TN-C-S (PME) supply ------------- */
    if (s_earthing == EVSE_EARTH_TN_C_S) {
        if (debounce(pen_fault_active(), &s_pen_count)) {
            fault_raise(EVSE_FAULT_PEN);
        } else if (s_pen_count == 0) {
            fault_clr(EVSE_FAULT_PEN);
        }
    }

    /* --- Weld detect, only meaningful while commanded open -------------- */
    if (!relay_is_closed()) {
        if (debounce(weld_detect_active(), &s_weld_count)) {
            fault_raise(EVSE_FAULT_RELAY_WELD);
        }
    } else {
        s_weld_count = 0;
    }

    /* --- Thermal -------------------------------------------------------- */
#if EVSE_TEMP_SENSORS_FITTED
    update_temperatures();
    int16_t hottest = (s_temp_conn_c > s_temp_int_c) ? s_temp_conn_c : s_temp_int_c;
    if (hottest >= TEMP_TRIP_C) {
        fault_raise(EVSE_FAULT_OVER_TEMP);
    } else if (hottest < (TEMP_TRIP_C - 10)) {
        /* 10 C of hysteresis so we do not chatter around the trip point. */
        fault_clr(EVSE_FAULT_OVER_TEMP);
    }
#endif

    /* --- Metering-derived checks ---------------------------------------- */
    if (meter == NULL || !meter->valid) {
        fault_set(EVSE_FAULT_METER_COMM);
        s_oc_hard_since = s_oc_soft_since = s_uv_since = s_ov_since = 0;
        return;
    }
    fault_clr(EVSE_FAULT_METER_COMM);

    if (sustained(meter->voltage_v < VOLTAGE_MIN_V, &s_uv_since, VOLTAGE_FAULT_MS)) {
        fault_raise(EVSE_FAULT_UNDER_VOLTAGE);
    } else if (s_uv_since == 0) {
        fault_clr(EVSE_FAULT_UNDER_VOLTAGE);
    }

    if (sustained(meter->voltage_v > VOLTAGE_MAX_V, &s_ov_since, VOLTAGE_FAULT_MS)) {
        fault_raise(EVSE_FAULT_OVER_VOLTAGE);
    } else if (s_ov_since == 0) {
        fault_clr(EVSE_FAULT_OVER_VOLTAGE);
    }

    /*
     * Over-current is only checked while we are actually supplying, and only
     * against an offer big enough for the ratio to mean something.
     */
    if (charging && offered_a >= OC_MIN_MEANINGFUL_A) {
        bool hard = meter->current_a > offered_a * OC_HARD_RATIO;
        bool soft = meter->current_a > offered_a * OC_SOFT_RATIO;
        if (sustained(hard, &s_oc_hard_since, OC_HARD_MS) ||
            sustained(soft, &s_oc_soft_since, OC_SOFT_MS)) {
            fault_raise(EVSE_FAULT_OVER_CURRENT);
        }
    } else {
        s_oc_hard_since = s_oc_soft_since = 0;
        fault_clr(EVSE_FAULT_OVER_CURRENT);
    }
}

uint32_t safety_clear_recoverable(void)
{
    /*
     * Only clear what we can currently prove is gone. Conditions that are still
     * asserted will simply be re-raised by the next safety_update(), but not
     * clearing them here avoids a window where the state machine sees a clean
     * mask and re-arms.
     */
    if (!estop_active())                        fault_clr(EVSE_FAULT_ESTOP);
    if (pe_present())                           fault_clr(EVSE_FAULT_PE_LOST);
    if (!pen_fault_active())                    fault_clr(EVSE_FAULT_PEN);
    if (!rcd_int_active()) { s_rcd_tripped = false; fault_clr(EVSE_FAULT_RCD_TRIP); }

#if EVSE_TEMP_SENSORS_FITTED
    int16_t hottest = (s_temp_conn_c > s_temp_int_c) ? s_temp_conn_c : s_temp_int_c;
    if (hottest < (TEMP_TRIP_C - 10))           fault_clr(EVSE_FAULT_OVER_TEMP);
#endif

    fault_clr(EVSE_FAULT_OVER_CURRENT | EVSE_FAULT_CP_INVALID |
              EVSE_FAULT_CP_DIODE     | EVSE_FAULT_CP_SHORT   |
              EVSE_FAULT_VENT_REQUIRED);

    return s_faults;
}

bool safety_rcd_selftest(void)
{
    if (relay_is_closed()) {
        return false;      /* must not be run while supplying */
    }

    s_rcd_tripped = false;
    fault_clr(EVSE_FAULT_RCD_TRIP);

    HAL_GPIO_WritePin(RCD_TEST_GPIO_PORT, RCD_TEST_GPIO_PIN, RCD_TEST_ACTIVE_STATE);

    bool tripped = false;
    for (uint32_t waited = 0; waited < RCD_TEST_TIMEOUT_MS; waited += 5u) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (s_rcd_tripped || rcd_int_active()) { tripped = true; break; }
    }

    HAL_GPIO_WritePin(RCD_TEST_GPIO_PORT, RCD_TEST_GPIO_PIN,
                      (RCD_TEST_ACTIVE_STATE == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET);

    /* Give the RCD IC time to release its latch before we judge the result. */
    vTaskDelay(pdMS_TO_TICKS(50));
    s_rcd_tripped = false;
    fault_clr(EVSE_FAULT_RCD_TRIP);

    if (!tripped) {
        /*
         * The RCD did not respond to a deliberate fault current. There is no
         * residual-current protection behind the contactor, so this latches.
         */
        fault_raise(EVSE_FAULT_RCD_SELFTEST);
        return false;
    }

    /* It should have released once the injection stopped; if not, it is stuck. */
    if (rcd_int_active()) {
        fault_raise(EVSE_FAULT_RCD_SELFTEST);
        return false;
    }
    return true;
}

void safety_selftest(safety_selftest_result_t *out)
{
    safety_selftest_result_t r = {0};

    r.pe_ok  = pe_present();
    r.pen_ok = (s_earthing != EVSE_EARTH_TN_C_S) || !pen_fault_active();
    r.relay_open_ok  = (relay_check_weld() == RELAY_OK);
    r.rcd_selftest_ok = safety_rcd_selftest();

    if (!r.pe_ok)          fault_raise(EVSE_FAULT_PE_LOST);
    if (!r.pen_ok)         fault_raise(EVSE_FAULT_PEN);
    if (!r.relay_open_ok)  fault_raise(EVSE_FAULT_RELAY_WELD);

    /* meter_ok and pilot_ok are filled in by the caller, which owns those. */
    r.passed = r.pe_ok && r.pen_ok && r.relay_open_ok && r.rcd_selftest_ok;

    if (out) *out = r;
}
