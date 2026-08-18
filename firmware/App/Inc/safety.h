/**
 * @file  safety.h
 * @brief Protection supervisor: RCD, PEN, PE, weld, E-stop, thermal, over-current.
 *
 * Runs at TASK_PRIO_SAFETY, the highest priority in the system, every
 * TASK_PERIOD_SAFETY_MS. It owns the trip path and can open the contactor on its
 * own without asking the charging state machine — see the note in
 * docs/ARCHITECTURE.md §3.1.
 *
 * The RCD is not polled. Its trip output is an EXTI whose handler opens the
 * contactor directly, so the electrical trip latency is a few microseconds and
 * is independent of scheduling. The 5 ms poll only serves the non-latching
 * inputs and acts as a backstop.
 */
#ifndef SAFETY_H
#define SAFETY_H

#include "evse_types.h"

/** Earthing arrangement of the installation — decides which checks are armed. */
typedef enum {
    EVSE_EARTH_TN_S = 0,   /**< separate PE conductor: PE detect only        */
    EVSE_EARTH_TN_C_S,     /**< PME/TN-C-S: PEN fault detection required     */
    EVSE_EARTH_TT,         /**< local earth electrode: PE detect only        */
    EVSE_EARTH_SPLIT_PHASE /**< North-American 120/240 V: PE detect only     */
} evse_earthing_t;

/** Result of the power-on self-test. */
typedef struct {
    bool relay_open_ok;    /**< contactor proven open (no weld)              */
    bool rcd_selftest_ok;  /**< RCD tripped when we injected a test current  */
    bool pe_ok;
    bool pen_ok;
    bool meter_ok;
    bool pilot_ok;
    bool passed;           /**< all of the above                             */
} safety_selftest_result_t;

void safety_init(evse_earthing_t earthing);

/**
 * One supervision pass. Call from the safety task only.
 * @param meter     latest metering snapshot, or NULL if metering is down
 * @param offered_a current currently advertised on the pilot, amps
 * @param charging  true while the contactor is expected to be closed
 */
void safety_update(const meter_reading_t *meter, float offered_a, bool charging);

/** Current fault bitmask (bitwise OR of evse_fault_t). */
uint32_t safety_get_faults(void);

/** True if any fault in EVSE_FAULT_LATCHING_MASK is set. */
bool safety_is_latched(void);

/**
 * Clear the non-latching faults whose underlying condition has gone away.
 * Latching faults are untouched — those need a power cycle or a service action.
 * @return the fault mask remaining after the attempt.
 */
uint32_t safety_clear_recoverable(void);

/**
 * Full power-on self-test. Blocks for up to ~1 s; call from the EVSE task
 * during EVSE_STATE_BOOT, with the contactor open.
 */
void safety_selftest(safety_selftest_result_t *out);

/**
 * Fire the RCD self-test coil and confirm the device trips.
 * IEC 62752/62955 want this at least daily and before energising after a long
 * idle. Blocks for up to RCD_TEST_TIMEOUT_MS. Must be called with the contactor
 * open — it deliberately trips the RCD.
 */
bool safety_rcd_selftest(void);

/** Thermal derate factor, 0.0 (stop) .. 1.0 (no derate). */
float safety_thermal_derate(void);

/** Latest temperatures in degrees Celsius. Either pointer may be NULL. */
void safety_get_temps(int16_t *connector_c, int16_t *internal_c);

/** EXTI callback hook for the RCD trip line. Opens the contactor immediately. */
void safety_rcd_isr(void);
/** EXTI callback hook for the emergency-off button. */
void safety_estop_isr(void);

#endif /* SAFETY_H */
