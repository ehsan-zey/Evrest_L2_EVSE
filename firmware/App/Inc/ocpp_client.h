/**
 * @file  ocpp_client.h
 * @brief OCPP 1.6J charge point client.
 *
 * Implements the **Core** profile in full, plus **SmartCharging**,
 * **LocalAuthListManagement**, **Reservation** and **RemoteTrigger**.
 *
 * Structure:
 *   - one task owns the WebSocket and all OCPP state, so nothing else needs a
 *     lock to talk to the CSMS;
 *   - outbound messages go through a queue, so a caller in the EVSE task never
 *     blocks on the network;
 *   - CALLs awaiting a response live in a small table keyed by message id, with
 *     a timeout, because a CSMS that never answers must not leak the slot.
 *
 * Offline behaviour is the part that matters most in the field: transactions
 * are journalled and replayed with their original timestamps once the link
 * returns, so a site with flaky backhaul still bills correctly.
 */
#ifndef OCPP_CLIENT_H
#define OCPP_CLIENT_H

#include "evse_sm.h"
#include "ocpp_auth.h"

/** Create the OCPP task's state and allow it to start connecting. */
void ocpp_client_start(void);

/** FreeRTOS entry point; created by evse_app.c. */
void task_ocpp(void *arg);

/** True once BootNotification has been accepted. */
bool ocpp_is_connected(void);

/** Callbacks the state machine calls to raise OCPP events. */
const evse_sm_callbacks_t *ocpp_get_sm_callbacks(void);

/** Force a reconnect, e.g. after the CSMS URL changes. */
void ocpp_client_reconnect(void);

/** Human-readable connection state, for the console and the HMI. */
const char *ocpp_state_name(void);

#endif /* OCPP_CLIENT_H */
