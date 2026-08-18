/**
 * @file  txn_log.h
 * @brief Flash-backed transaction journal for offline operation.
 *
 * A charger on a site with poor backhaul still has to bill correctly. Every
 * transaction event is appended to a flash ring **before** it is transmitted,
 * and replayed with its **original** timestamps once the CSMS is reachable.
 * Re-stamping on replay would move the session to whenever the link came back,
 * which is exactly the data a billing dispute turns on.
 *
 * Flash cannot be rewritten in place, so "this record has been acknowledged"
 * is itself an appended record rather than a flag edited into the original.
 * On boot the log is scanned and any START/STOP with a matching ACK is skipped.
 * The region is only erased when it fills, and then only after the still
 * unacknowledged records have been copied forward.
 */
#ifndef TXN_LOG_H
#define TXN_LOG_H

#include "evse_types.h"
#include <stddef.h>

void txn_log_init(void);

/** Journal the start of a transaction. Call before StartTransaction is sent. */
void txn_log_record_start(const evse_transaction_t *txn);

/** Journal the end of a transaction. Call before StopTransaction is sent. */
void txn_log_record_stop(const evse_transaction_t *txn);

/** Record the CSMS-assigned transaction id against a journalled start. */
void txn_log_set_transaction_id(uint32_t local_id, int32_t transaction_id);

/** Mark a transaction fully acknowledged; it will not be replayed again. */
void txn_log_confirm_stop(uint32_t local_id);

/** Begin replaying unacknowledged records. Called when the CSMS accepts boot. */
void txn_log_replay_begin(void);

/**
 * Emit at most one queued record. Called from the OCPP task loop so replay
 * shares the outbound queue with live traffic instead of flooding it.
 * @return true if a record was emitted.
 */
bool txn_log_replay_step(void);

/** Unacknowledged records still in the log. */
size_t txn_log_pending_count(void);

#endif /* TXN_LOG_H */
