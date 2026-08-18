/**
 * @file  cp_pilot.c
 * @brief Control pilot implementation. See cp_pilot.h for the design notes.
 */
#include "cp_pilot.h"
#include "evse_board.h"
#include <string.h>
#include <stdint.h>

#ifndef EVSE_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#endif

/* ======================================================================== */
/* Pure logic                                                               */
/* ======================================================================== */

cp_state_t cp_decode_state(int32_t mv)
{
    if (mv > CP_ABS_MAX_MV || mv < -CP_ABS_MAX_MV) return CP_STATE_INVALID;
    if (mv >= CP_BOUND_A_B_MV) return CP_STATE_A;
    if (mv >= CP_BOUND_B_C_MV) return CP_STATE_B;
    if (mv >= CP_BOUND_C_D_MV) return CP_STATE_C;
    if (mv >= CP_BOUND_D_E_MV) return CP_STATE_D;
    if (mv >= CP_BOUND_E_F_MV) return CP_STATE_E;
    return CP_STATE_F;
}

float cp_duty_for_current(float amps)
{
    if (amps < EVSE_MIN_CURRENT_A) {
        return 0.0f;                       /* caller must drive static +12 V */
    }
    if (amps > 80.0f) {
        amps = 80.0f;                      /* Table A.7 tops out at 80 A     */
    }
    /* 10 % .. 85 % encodes 6 A .. 51 A at 0.6 A per percent. */
    if (amps <= 51.0f) {
        return amps / 0.6f;
    }
    /* 85 % .. 96 % encodes 53 A .. 80 A at 2.5 A per percent. */
    return (amps / 2.5f) + 64.0f;
}

bool cp_duty_is_digital_comm(float duty_pct)
{
    return (duty_pct >= 3.0f && duty_pct <= 7.0f);
}

float cp_current_for_duty(float duty_pct)
{
    /* IEC 61851-1 Table A.7, including the gaps that carry no meaning. */
    if (duty_pct < 3.0f)   return 0.0f;   /* not allowed                     */
    if (duty_pct <= 7.0f)  return 0.0f;   /* digital communication only      */
    if (duty_pct < 8.0f)   return 0.0f;   /* not allowed                     */
    if (duty_pct < 10.0f)  return 6.0f;   /* 8..10 % is a flat 6 A           */
    if (duty_pct <= 85.0f) return duty_pct * 0.6f;
    if (duty_pct <= 96.0f) return (duty_pct - 64.0f) * 2.5f;
    if (duty_pct <= 97.0f) return 80.0f;
    return 0.0f;                          /* >97 % is not a current offer    */
}

/* Front-end calibration, seeded from evse_board.h and overridable from NVM. */
static int32_t s_cal_num    = CP_ADC_SCALE_NUM;
static int32_t s_cal_den    = CP_ADC_SCALE_DEN;
static int32_t s_cal_offset = CP_ADC_OFFSET_MV;

void cp_pilot_set_calibration(int32_t num, int32_t den, int32_t offset_mv)
{
    if (den == 0) {
        s_cal_num = CP_ADC_SCALE_NUM;
        s_cal_den = CP_ADC_SCALE_DEN;
        s_cal_offset = CP_ADC_OFFSET_MV;
        return;
    }
    s_cal_num = num;
    s_cal_den = den;
    s_cal_offset = offset_mv;
}

int32_t cp_adc_to_mv(uint16_t adc_raw)
{
    /*
     * ADC code -> millivolts at the pin -> CP volts, in one expression.
     *
     * Converting to millivolts first and scaling afterwards truncates twice:
     * the reference is 3300 mV over 4095 codes, so an intermediate in whole
     * millivolts quantises to 0.8 mV per code and adjacent codes collapse onto
     * the same value before the ~5.3x front-end gain is applied. Folding both
     * steps into a single 64-bit ratio keeps one ADC code worth about 4.3 mV
     * of CP voltage, which is what the median filter needs to resolve.
     */
    int64_t numer = (int64_t)adc_raw * 3300 * s_cal_num;
    int64_t denom = (int64_t)4095 * s_cal_den;
    return (int32_t)(numer / denom) - s_cal_offset;
}

/** In-place insertion sort median. CP_SAMPLE_DEPTH is small; this is fine. */
uint16_t cp_median_u16(uint16_t *buf, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        uint16_t key = buf[i];
        size_t j = i;
        while (j > 0 && buf[j - 1] > key) { buf[j] = buf[j - 1]; j--; }
        buf[j] = key;
    }
    return buf[n / 2];
}

/* ======================================================================== */
/* Hardware state                                                           */
/* ======================================================================== */

#ifndef EVSE_HOST_TEST

/** Fraction into each plateau at which we sample, in 1/256ths. */
#define CP_SAMPLE_POINT_Q8      192u   /* 75 % */

extern ADC_HandleTypeDef  hadc1;
extern TIM_HandleTypeDef  htim1;

/** DMA target for the regular (high plateau) group. Circular. */
static volatile uint16_t s_hi_samples[CP_SAMPLE_DEPTH];
/** Filled by the injected-conversion ISR. */
static volatile uint16_t s_lo_samples[CP_SAMPLE_DEPTH];
static volatile uint32_t s_lo_write_idx;
static volatile uint16_t s_pp_raw;

static cp_status_t   s_status;
static cp_state_t    s_candidate = CP_STATE_INVALID;
static uint32_t      s_candidate_count;
static uint32_t      s_arr;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_mem;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/**
 * Reposition the two ADC trigger compares for the current duty cycle.
 *
 * The high plateau runs [0, ccr1) and the low plateau [ccr1, arr]. We sample
 * 75 % into each. At the extremes one plateau becomes very short — at 96 % duty
 * the low plateau is only 40 us — so the trigger is additionally clamped away
 * from the edges by CP_EDGE_GUARD_TICKS to stay clear of the slew.
 */
#define CP_EDGE_GUARD_TICKS   50u        /* 2 us at the 25 MHz timer clock */

static void cp_place_sample_triggers(uint32_t ccr1)
{
    uint32_t arr = s_arr;
    uint32_t hi_len = ccr1;
    uint32_t lo_len = (arr + 1u > ccr1) ? (arr + 1u - ccr1) : 0u;

    if (hi_len > 2u * CP_EDGE_GUARD_TICKS) {
        uint32_t p = (hi_len * CP_SAMPLE_POINT_Q8) >> 8;
        if (p > hi_len - CP_EDGE_GUARD_TICKS) p = hi_len - CP_EDGE_GUARD_TICKS;
        if (p < CP_EDGE_GUARD_TICKS)          p = CP_EDGE_GUARD_TICKS;
        __HAL_TIM_SET_COMPARE(&htim1, CP_SAMPLE_HI_CHANNEL, p);
    } else {
        /* No usable high plateau (duty ~0). Park the trigger; samples stale. */
        __HAL_TIM_SET_COMPARE(&htim1, CP_SAMPLE_HI_CHANNEL, 0u);
    }

    if (lo_len > 2u * CP_EDGE_GUARD_TICKS) {
        uint32_t p = ccr1 + ((lo_len * CP_SAMPLE_POINT_Q8) >> 8);
        if (p > arr - CP_EDGE_GUARD_TICKS) p = arr - CP_EDGE_GUARD_TICKS;
        __HAL_TIM_SET_COMPARE(&htim1, CP_SAMPLE_LO_CHANNEL, p);
    } else {
        __HAL_TIM_SET_COMPARE(&htim1, CP_SAMPLE_LO_CHANNEL, arr);
    }
}

static void cp_apply_duty(float duty_pct)
{
    uint32_t ccr1;

    if (duty_pct >= 100.0f) {
        ccr1 = s_arr + 1u;                      /* permanently high: +12 V   */
    } else if (duty_pct <= 0.0f) {
        ccr1 = 0u;                              /* permanently low:  -12 V   */
    } else {
        ccr1 = (uint32_t)((duty_pct / 100.0f) * (float)(s_arr + 1u) + 0.5f);
    }
    __HAL_TIM_SET_COMPARE(&htim1, CP_PWM_CHANNEL, ccr1);
    cp_place_sample_triggers(ccr1);
}

bool cp_pilot_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.state     = CP_STATE_INVALID;
    s_status.raw_state = CP_STATE_INVALID;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);

    s_arr = __HAL_TIM_GET_AUTORELOAD(&htim1);
    if (s_arr == 0u) {
        return false;                 /* timer was never initialised */
    }

    /* Idle = static +12 V, i.e. state A with no modulation. */
    s_status.pwm_active = false;
    s_status.duty_pct   = 100.0f;
    cp_apply_duty(100.0f);

    if (HAL_TIM_PWM_Start(&htim1, CP_PWM_CHANNEL) != HAL_OK)          return false;
    if (HAL_TIM_OC_Start(&htim1, CP_SAMPLE_HI_CHANNEL) != HAL_OK)     return false;
    if (HAL_TIM_OC_Start(&htim1, CP_SAMPLE_LO_CHANNEL) != HAL_OK)     return false;

    /*
     * TIM1 is an advanced timer: without MOE the pilot output stays inert no
     * matter what the compare registers say. Any break event also clears it,
     * which is why cp_pilot_update() re-checks it below.
     */
    __HAL_TIM_MOE_ENABLE(&htim1);

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET,
                                    ADC_SINGLE_ENDED) != HAL_OK)      return false;
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_hi_samples,
                          CP_SAMPLE_DEPTH) != HAL_OK)                 return false;
    if (HAL_ADCEx_InjectedStart_IT(&hadc1) != HAL_OK)                 return false;

    return true;
}

void cp_pilot_injected_isr(uint16_t cp_low, uint16_t pp)
{
    uint32_t i = s_lo_write_idx;
    s_lo_samples[i] = cp_low;
    s_lo_write_idx = (i + 1u) % CP_SAMPLE_DEPTH;
    s_pp_raw = pp;
}

uint16_t cp_pilot_pp_raw(void) { return s_pp_raw; }

void cp_pilot_set_offer(float amps)
{
    float duty = cp_duty_for_current(amps);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (duty <= 0.0f) {
        /*
         * Below 6 A there is no legal duty cycle. Hold a static +12 V: the
         * vehicle stays connected and sees "not authorised" rather than the
         * pilot disappearing, which some vehicles latch as a fault.
         */
        s_status.pwm_active = false;
        s_status.duty_pct   = 100.0f;
        s_status.offered_a  = 0.0f;
        cp_apply_duty(100.0f);
    } else {
        s_status.pwm_active = true;
        s_status.duty_pct   = duty;
        s_status.offered_a  = cp_current_for_duty(duty);
        cp_apply_duty(duty);
    }
    xSemaphoreGive(s_lock);
}

void cp_pilot_set_static(cp_state_t level)
{
    if (level != CP_STATE_A && level != CP_STATE_F) {
        return;   /* the EVSE can only source +12 V and -12 V */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.pwm_active = false;
    s_status.offered_a  = 0.0f;
    s_status.duty_pct   = (level == CP_STATE_A) ? 100.0f : 0.0f;
    cp_apply_duty(s_status.duty_pct);
    xSemaphoreGive(s_lock);
}

void cp_pilot_disable(void)
{
    cp_pilot_set_static(CP_STATE_F);
}

void cp_pilot_update(void)
{
    uint16_t hi_copy[CP_SAMPLE_DEPTH];
    uint16_t lo_copy[CP_SAMPLE_DEPTH];

    /*
     * The DMA and the injected ISR write these rings concurrently. A torn read
     * would at worst mix samples from adjacent periods, and the median filter
     * absorbs that, so a plain copy is sufficient — no need to stall the DMA.
     */
    for (size_t i = 0; i < CP_SAMPLE_DEPTH; i++) {
        hi_copy[i] = s_hi_samples[i];
        lo_copy[i] = s_lo_samples[i];
    }

    /*
     * Re-assert MOE. A break event on the advanced timer clears it silently,
     * which would drop the pilot to its idle level while every compare register
     * still reads correct — the failure would otherwise be invisible from here.
     */
    __HAL_TIM_MOE_ENABLE(&htim1);

    int32_t v_hi = cp_adc_to_mv(cp_median_u16(hi_copy, CP_SAMPLE_DEPTH));
    int32_t v_lo = cp_adc_to_mv(cp_median_u16(lo_copy, CP_SAMPLE_DEPTH));

    cp_state_t decoded = cp_decode_state(v_hi);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_status.v_high_mv = (int16_t)evse_clampf((float)v_hi, -32000.0f, 32000.0f);
    s_status.v_low_mv  = (int16_t)evse_clampf((float)v_lo, -32000.0f, 32000.0f);
    s_status.raw_state = decoded;

    /*
     * The diode check only means anything while we are modulating — with a
     * static pilot there is no negative plateau to look at, so we hold the last
     * verdict rather than reporting a missing diode.
     */
    if (s_status.pwm_active) {
        s_status.diode_ok = (v_lo <= CP_DIODE_THRESHOLD_MV);
    }

    if (decoded == s_candidate) {
        if (s_candidate_count < CP_DEBOUNCE_COUNT) {
            s_candidate_count++;
        }
    } else {
        s_candidate = decoded;
        s_candidate_count = 1u;
    }

    if (s_candidate_count >= CP_DEBOUNCE_COUNT && s_status.state != s_candidate) {
        s_status.state = s_candidate;
        s_status.state_since_ms = now_ms();
    }

    xSemaphoreGive(s_lock);
}

void cp_pilot_get(cp_status_t *out)
{
    if (out == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

#endif /* !EVSE_HOST_TEST */
