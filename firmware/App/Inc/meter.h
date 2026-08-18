/**
 * @file  meter.h
 * @brief ATM90E26 single-phase energy metering front-end, UART4.
 *
 * The ATM90E26 sits behind the isolated supply and speaks its serial protocol
 * at 9600 8N1. It provides Urms, Irms, mean active/reactive/apparent power,
 * power factor, line frequency and energy registers.
 *
 * See docs/METERING.md for what the IRQ line can and cannot do.
 */
#ifndef METER_H
#define METER_H

#include "evse_types.h"

/** Result of the startup probe, useful in the self-test report. */
typedef enum {
    METER_INIT_OK = 0,
    METER_INIT_NO_RESPONSE,   /**< no reply on the UART                    */
    METER_INIT_BAD_CHECKSUM,  /**< replies arrive but are corrupt          */
    METER_INIT_CAL_REJECTED   /**< the IC rejected the calibration write   */
} meter_init_result_t;

/**
 * Probe the ATM90E26, apply metering calibration and arm the sag interrupt.
 * @param sag_threshold_v  voltage below which the IC raises IRQ
 */
meter_init_result_t meter_init(float sag_threshold_v);

/**
 * Read the IC and integrate energy. Call at TASK_PERIOD_METER_MS.
 * A failed transfer keeps the previous reading but marks it invalid, so a
 * transient UART glitch does not look like the load vanishing.
 */
void meter_update(void);

/** Latest reading. Safe from any task. */
void meter_get(meter_reading_t *out);

/**
 * Fast current read for the safety task, bypassing the full update.
 * Returns the last known Irms without touching the UART, so it is cheap enough
 * to call every 5 ms.
 */
float meter_last_current_a(void);

/** Lifetime imported energy, watt-hours. Restored from NVM at boot. */
uint64_t meter_energy_wh(void);
void     meter_set_energy_wh(uint64_t wh);

/** True if the last few transfers all failed. */
bool meter_is_faulted(void);

/**
 * Read and clear the ATM90E26 SysStatus register after an IRQ.
 * @param sag_out    set true if the sag warning is present
 * @param revp_out   set true if active energy direction reversed
 * @return true if the register was read successfully
 */
bool meter_read_status(bool *sag_out, bool *revp_out);

/**
 * Consume a latched voltage-sag event, if one occurred since the last call.
 * The safety supervisor polls this; a sag means the supply cannot hold up the
 * load and the contactor should open.
 */
bool meter_take_sag_event(void);

/** EXTI hook for ENME_IRQ. Records the event; the meter task services it. */
void meter_irq_isr(void);

#endif /* METER_H */
