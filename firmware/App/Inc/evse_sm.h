/**
 * @file  evse_sm.h
 * @brief Charging state machine — the policy layer above pilot, relay and safety.
 *
 * Runs in the EVSE task at TASK_PERIOD_EVSE_MS. It decides *whether* to supply;
 * safety.c decides whether supply is permitted at all and can veto by opening
 * the contactor underneath it. The state machine observes that and follows,
 * rather than the two negotiating.
 *
 * Timing budget for "vehicle leaves state C" to "contactor open", which
 * IEC 61851-1 caps at 100 ms:
 *
 *   pilot debounce  3 x 10 ms  = 30 ms
 *   EVSE task period            = 20 ms  (worst case)
 *   GPIO write in relay_open()  =  0 ms
 *   ------------------------------------
 *   worst case                  = 50 ms
 *
 * The contactor's own drop-out time (RELAY_OPEN_TIME_MS, 20 ms) then applies,
 * giving 70 ms against a 100 ms limit.
 */
#ifndef EVSE_SM_H
#define EVSE_SM_H

#include "evse_types.h"

/** Reason an authorisation attempt was resolved. */
typedef enum {
    AUTH_PENDING = 0,
    AUTH_ACCEPTED,
    AUTH_REJECTED,
    AUTH_EXPIRED,
    AUTH_BLOCKED
} auth_result_t;

/**
 * Hooks into the OCPP layer. All are called from the EVSE task, so they must
 * not block for long; the OCPP client queues rather than transmits inline.
 */
typedef struct {
    /** State changed — emit StatusNotification. */
    void (*on_state_change)(evse_state_t from, evse_state_t to, uint32_t faults);
    /** A transaction is starting — emit StartTransaction. */
    void (*on_txn_start)(const evse_transaction_t *txn);
    /** A transaction has ended — emit StopTransaction. */
    void (*on_txn_stop)(const evse_transaction_t *txn);
    /** Ask the CSMS (or the local list) to authorise a tag. */
    void (*request_authorize)(const char *id_tag);
} evse_sm_callbacks_t;

void evse_sm_init(const evse_sm_callbacks_t *cb);

/** One state-machine pass. Call from the EVSE task only. */
void evse_sm_update(void);

/** Snapshot of everything the HMI and OCPP layers need. */
void evse_sm_get_status(evse_status_t *out);

/* ---- Inputs from the local reader and from OCPP ---- */

/**
 * Present a tag, either from a local RFID reader or from RemoteStartTransaction.
 * Starts the authorisation flow; the result arrives via evse_sm_auth_result().
 */
void evse_sm_present_tag(const char *id_tag);

/** Deliver the outcome of a pending authorisation. */
void evse_sm_auth_result(const char *id_tag, auth_result_t result);

/**
 * Request a stop. @p id_tag may be NULL for a remote or system stop; when it is
 * supplied it must match the tag that started the transaction, or a tag in the
 * same parent group, otherwise the request is refused.
 * @return true if a transaction was running and is now stopping.
 */
bool evse_sm_request_stop(const char *id_tag, stop_reason_t reason);

/**
 * Apply the smart-charging limit, amps. Pass a negative value to clear it.
 * A limit below EVSE_MIN_CURRENT_A suspends supply rather than offering an
 * illegal duty cycle — that is what SUSPENDED_EVSE means.
 */
void evse_sm_set_profile_limit(float amps);

/** ChangeAvailability: false takes the connector out of service when idle. */
void evse_sm_set_available(bool available);

/** Hold the connector for a reservation until @p expiry (Unix seconds). */
bool evse_sm_reserve(int32_t reservation_id, const char *id_tag, uint32_t expiry);
void evse_sm_cancel_reservation(int32_t reservation_id);

/**
 * Attempt to clear a recoverable fault. Latching faults are not cleared.
 * @return the fault mask that remains.
 */
uint32_t evse_sm_try_clear_fault(void);

/** True while a transaction is open. */
bool evse_sm_transaction_active(void);

/** Assign the CSMS-allocated transaction id once StartTransaction.conf lands. */
void evse_sm_set_transaction_id(uint32_t local_id, int32_t transaction_id);

#endif /* EVSE_SM_H */
