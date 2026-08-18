/**
 * @file  relay.h
 * @brief Contactor control with weld and failure-to-close detection.
 *
 * The pinout exposes one drive line ("RELAY_1", PD1) feeding both poles, so
 * the poles cannot be verified independently. "WELD_DET" — which senses mains
 * downstream of the contactor — is therefore the only contactor integrity
 * check available, and it gives us both directions:
 *
 *   commanded OPEN   + weld detect asserted -> a contact is welded closed
 *   commanded CLOSED + weld detect clear    -> the contactor failed to close
 *
 * Because a welded pole cannot be distinguished from a welded pair here, any
 * weld indication is treated as a latching fault.
 */
#ifndef RELAY_H
#define RELAY_H

#include "evse_types.h"

typedef enum {
    RELAY_OK = 0,
    RELAY_ERR_WELDED,      /**< downstream live while commanded open   */
    RELAY_ERR_NO_CLOSE,    /**< downstream dead while commanded closed */
    RELAY_ERR_INHIBITED    /**< a latching fault forbids energising    */
} relay_result_t;

void relay_init(void);

/**
 * Close the contactor and verify.
 *
 * Sequence: refuse if inhibited, prove open via the weld check, energise, wait
 * RELAY_CLOSE_TIME_MS plus a mains cycle, then confirm downstream is live.
 * On any failure the contactor is opened again before returning.
 *
 * Blocks for roughly 70 ms. Call from the EVSE task, never from the safety task.
 */
relay_result_t relay_close(void);

/** Open the contactor and confirm the downstream side goes dead. */
relay_result_t relay_open(void);

/**
 * Open immediately with a single GPIO write.
 *
 * ISR- and fault-path safe: no locks, no allocation, no blocking, no
 * verification. This is what the RCD and emergency-off interrupts call, and
 * what bounds the trip latency. Everything else should use relay_open().
 */
void relay_emergency_open(void);

/** True while the contactor is commanded closed. */
bool relay_is_closed(void);

/**
 * Weld check, valid only while the contactor is open. Samples across a full
 * mains cycle because the sense signal is derived from the AC line.
 * Blocks for ~24 ms.
 */
relay_result_t relay_check_weld(void);

/** Latch the contactor out until reboot. Called on any latching fault. */
void relay_inhibit(void);

/** True once relay_inhibit() has been called. */
bool relay_is_inhibited(void);

#endif /* RELAY_H */
