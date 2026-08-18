/**
 * @file  cp_pilot.h
 * @brief IEC 61851-1 / SAE J1772 control pilot generation and decoding.
 *
 * The EVSE drives a 1 kHz ±12 V square wave onto CP. The vehicle loads the
 * positive half with a resistor; the resulting plateau voltage encodes its
 * state, and the duty cycle we drive tells the vehicle how much current it may
 * take.
 *
 * Sampling is synchronous with the PWM. ADC1 carries two groups:
 *
 *   regular  (DMA, circular)  <- TIM1_CH2, 75 % into the HIGH plateau
 *   injected (interrupt)      <- TIM1_CH4, 75 % into the LOW plateau
 *                                rank 1 = CP, rank 2 = PP
 *
 * Sampling late in each plateau lets the CP network settle after the edge, and
 * both plateaus are median filtered over CP_SAMPLE_DEPTH periods before a
 * decision is made. That is what keeps contactor and switch-mode noise out of
 * the state decode.
 *
 * The proximity pilot rides along as injected rank 2 purely because it needs an
 * ADC and the injected group already has a trigger; PP is a DC level, so where
 * in the period it lands does not matter.
 */
#ifndef CP_PILOT_H
#define CP_PILOT_H

#include "evse_types.h"
#include <stddef.h>

/** Periods of history median-filtered before decoding. 16 ms of pilot. */
#define CP_SAMPLE_DEPTH        16u

/**
 * Decision boundaries, millivolts at the CP pin.
 *
 * IEC 61851-1 Table A.4 gives narrow acceptance windows around each nominal
 * (e.g. 8.36–9.56 V for state B) and leaves the gaps between them undefined.
 * Decoding strictly to those windows means any reading in a gap is discarded,
 * which on a noisy installation shows up as spurious "invalid pilot" faults.
 *
 * We instead split at the midpoints between adjacent nominals so every reading
 * inside the legal range decodes to something, and rely on the median filter
 * plus CP_DEBOUNCE_COUNT to reject noise. Readings outside ±13 V are physically
 * impossible on a working front-end and are reported as invalid.
 */
#define CP_BOUND_A_B_MV        10500   /* 12 V / 9 V  midpoint */
#define CP_BOUND_B_C_MV         7500   /*  9 V / 6 V  midpoint */
#define CP_BOUND_C_D_MV         4500   /*  6 V / 3 V  midpoint */
#define CP_BOUND_D_E_MV         1500   /*  3 V / 0 V  midpoint */
#define CP_BOUND_E_F_MV        (-6000) /*  0 V / -12 V midpoint */
#define CP_ABS_MAX_MV          13500

/** Consecutive identical decodes required before cp_status_t::state changes. */
#define CP_DEBOUNCE_COUNT      3u

/** The vehicle's diode must pull the low plateau below this. */
#define CP_DIODE_THRESHOLD_MV  (-10000)

/* ======================================================================== */
/* Pure logic — no hardware, unit-tested on the host in firmware/test/       */
/* ======================================================================== */

/**
 * Decode a CP high-plateau voltage into a pilot state.
 * @param mv  CP voltage in millivolts (signed, referenced to PE).
 */
cp_state_t cp_decode_state(int32_t mv);

/**
 * Duty cycle to advertise a given current, per IEC 61851-1 Table A.7.
 * @param amps  Desired offer.
 * @return Duty in percent, or 0.0f if @p amps is below the 6 A minimum (the
 *         caller must then drive a static level instead of PWM).
 */
float cp_duty_for_current(float amps);

/**
 * Inverse of cp_duty_for_current(), used to check what we are actually driving.
 * @return Amps, or 0.0f for duty cycles that do not encode a current
 *         (including the 5 % digital-communication signalling).
 */
float cp_current_for_duty(float duty_pct);

/** True if @p duty encodes the 5 % "digital communication only" request. */
bool cp_duty_is_digital_comm(float duty_pct);

/**
 * Median of @p n samples. Sorts @p buf in place, so pass a scratch copy.
 * This is what rejects switching transients from the plateau readings — a
 * mean would let a single spike drag the decoded state across a boundary.
 */
uint16_t cp_median_u16(uint16_t *buf, size_t n);

/**
 * Convert a raw ADC reading to CP millivolts using the front-end calibration.
 * @param adc_raw  12-bit right-aligned ADC code.
 */
int32_t cp_adc_to_mv(uint16_t adc_raw);

/* ======================================================================== */
/* Hardware-backed API                                                      */
/* ======================================================================== */

/** Start the pilot timer and ADC. Leaves the pilot in state A (static +12 V). */
bool cp_pilot_init(void);

/**
 * Advertise @p amps to the vehicle.
 *
 * Values at or above EVSE_MIN_CURRENT_A start PWM at the corresponding duty.
 * Values below it stop modulation and hold a static +12 V, which tells the
 * vehicle "connected but not authorised to charge" without dropping the pilot.
 */
void cp_pilot_set_offer(float amps);

/**
 * Drive a static pilot level rather than a current offer.
 * Only CP_STATE_A (+12 V, idle) and CP_STATE_F (−12 V, unavailable) are valid;
 * anything else is rejected because the EVSE cannot source those levels.
 */
void cp_pilot_set_static(cp_state_t level);

/**
 * Park the pilot at a steady -12 V (state F, "EVSE unavailable").
 * This board has no CP tri-state line, so -12 V is the strongest "do not
 * charge" the hardware can express.
 */
void cp_pilot_disable(void);

/** Most recent raw proximity-pilot ADC code. Decoded by proximity.c. */
uint16_t cp_pilot_pp_raw(void);

/**
 * Consume the sample rings, median filter, decode and debounce.
 * Called at TASK_PERIOD_PILOT_MS by the pilot task.
 */
void cp_pilot_update(void);

/** Latest debounced pilot status. Safe to call from any task. */
void cp_pilot_get(cp_status_t *out);

/**
 * ISR hook for the injected-conversion complete callback.
 * @param cp_low  injected rank 1 — CP negative plateau
 * @param pp      injected rank 2 — proximity pilot
 */
void cp_pilot_injected_isr(uint16_t cp_low, uint16_t pp);

/** Override the front-end calibration from NVM. Pass 0 for @p den to reset. */
void cp_pilot_set_calibration(int32_t num, int32_t den, int32_t offset_mv);

#endif /* CP_PILOT_H */
