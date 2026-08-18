/**
 * @file  evse_sm.c
 * @brief Charging state machine. See evse_sm.h.
 */
#include "evse_sm.h"
#include "evse_board.h"
#include "evse_config.h"
#include "cp_pilot.h"
#include "proximity.h"
#include "relay.h"
#include "safety.h"
#include "meter.h"
#include "hmi.h"
#include "rtc_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------- */
/* Tuning                                                                 */
/* ---------------------------------------------------------------------- */

/** How long the vehicle may sit in state B before we give up waiting. */
#define CONNECT_TIMEOUT_MS        (5u * 60u * 1000u)
/**
 * IEC 61851-1 gives the EVSE up to 3 s to close after the vehicle asks (state
 * C). We use a shorter settle so the pilot reading is stable first, then close.
 */
#define PRECLOSE_SETTLE_MS        200u
/** Vehicle taken no current for this long in state C: treat as suspended. */
#define EV_SUSPEND_MS             (60u * 1000u)
/** Retry a failed close this many times before faulting. */
#define CLOSE_RETRY_LIMIT         2u

static const evse_sm_callbacks_t *s_cb;
static evse_status_t   s_st;
static evse_transaction_t s_txn;

static uint32_t s_state_entered_ms;
static uint32_t s_zero_current_since_ms;
static uint8_t  s_close_retries;
static bool     s_available = true;
static float    s_profile_limit_a = -1.0f;   /* negative = no limit */
static uint32_t s_next_local_txn_id = 1u;

static char          s_pending_tag[OCPP_IDTAG_MAXLEN];
static auth_result_t s_auth_state;
static char          s_active_tag[OCPP_IDTAG_MAXLEN];

/* Reservation */
static int32_t  s_reservation_id = -1;
static char     s_reservation_tag[OCPP_IDTAG_MAXLEN];
static uint32_t s_reservation_expiry;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
static uint32_t in_state_ms(void) { return now_ms() - s_state_entered_ms; }

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static void enter_state(evse_state_t next)
{
    if (next == s_st.state) return;

    evse_state_t prev = s_st.state;
    s_st.state = next;
    s_state_entered_ms = now_ms();
    s_close_retries = 0;

    if (s_cb && s_cb->on_state_change) {
        s_cb->on_state_change(prev, next, s_st.faults);
    }
}

/**
 * Compute what we are allowed to offer the vehicle right now.
 *
 * Every input is a ceiling, so the answer is the minimum. Keeping the
 * individual limits in evse_status_t rather than just the result means the
 * dashboard can show *why* a car is charging slowly, which is the single most
 * common support question on a live site.
 */
static float compute_offer(void)
{
    float limit = EVSE_MAX_CURRENT_A;

    s_st.limit_hw_a      = EVSE_MAX_CURRENT_A;
    s_st.limit_install_a = cfg_dip_current_a();
    s_st.limit_thermal_a = EVSE_MAX_CURRENT_A * safety_thermal_derate();
    s_st.limit_profile_a = (s_profile_limit_a >= 0.0f)
                         ? s_profile_limit_a : EVSE_MAX_CURRENT_A;

    limit = evse_minf(limit, s_st.limit_install_a);
    limit = evse_minf(limit, s_st.limit_thermal_a);
    limit = evse_minf(limit, s_st.limit_profile_a);
    limit = evse_minf(limit, proximity_current_limit());

    return (limit < 0.0f) ? 0.0f : limit;
}

/** Open the contactor and close out any running transaction. */
static void stop_supply(stop_reason_t reason)
{
    if (relay_is_closed()) {
        (void)relay_open();
    }
    if (s_txn.active) {
        s_txn.active       = false;
        s_txn.stop_time    = rtc_unix_time();
        s_txn.meter_stop_wh = meter_energy_wh();
        s_txn.stop_reason  = reason;
        if (s_cb && s_cb->on_txn_stop) {
            s_cb->on_txn_stop(&s_txn);
        }
        hmi_play(HMI_TONE_CHARGE_END);
    }
    s_active_tag[0] = '\0';
}

static void start_transaction(const char *id_tag)
{
    memset(&s_txn, 0, sizeof(s_txn));
    s_txn.local_id       = s_next_local_txn_id++;
    s_txn.transaction_id = -1;              /* until the CSMS assigns one */
    s_txn.meter_start_wh = meter_energy_wh();
    s_txn.start_time     = rtc_unix_time();
    s_txn.active         = true;
    snprintf(s_txn.id_tag, sizeof(s_txn.id_tag), "%s", id_tag ? id_tag : "");

    if (s_cb && s_cb->on_txn_start) {
        s_cb->on_txn_start(&s_txn);
    }
}

/** True if the pilot is telling us the vehicle wants current. */
static bool vehicle_requests_charge(const cp_status_t *cp)
{
    if (cp->state == CP_STATE_C) return true;
    if (cp->state == CP_STATE_D) {
        /*
         * State D means the vehicle needs ventilation. Supplying without it is
         * a hydrogen accumulation risk, so this is refused unless the site has
         * been commissioned as ventilated.
         */
        return cfg_get()->ventilation_available;
    }
    return false;
}

/** Faults that should force us out of whatever we were doing. */
static bool fault_blocking(uint32_t faults)
{
    return (faults & (EVSE_FAULT_TRIP_MASK | EVSE_FAULT_LATCHING_MASK)) != 0u;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

void evse_sm_init(const evse_sm_callbacks_t *cb)
{
    s_cb = cb;
    memset(&s_st, 0, sizeof(s_st));
    memset(&s_txn, 0, sizeof(s_txn));
    s_st.state = EVSE_STATE_BOOT;
    s_state_entered_ms = now_ms();
    s_auth_state = AUTH_PENDING;
    s_pending_tag[0] = '\0';
    s_active_tag[0]  = '\0';
    s_profile_limit_a = -1.0f;
    s_available = true;

    /*
     * Latching faults are persisted, so a welded contactor or a failed RCD
     * self-test still holds the unit out of service after a power cycle. That
     * is the point of "latching" — a reset must not be a way to clear it.
     */
    uint32_t stored = cfg_get()->lockout_faults;
    if (stored != 0u) {
        s_st.faults |= stored;
        relay_inhibit();
    }
}

void evse_sm_present_tag(const char *id_tag)
{
    if (id_tag == NULL || id_tag[0] == '\0') return;

    /* A tag presented during a transaction is a stop request, not a start. */
    if (s_txn.active) {
        (void)evse_sm_request_stop(id_tag, STOP_REASON_LOCAL);
        return;
    }
    snprintf(s_pending_tag, sizeof(s_pending_tag), "%s", id_tag);
    s_auth_state = AUTH_PENDING;
    if (s_cb && s_cb->request_authorize) {
        s_cb->request_authorize(s_pending_tag);
    }
}

void evse_sm_auth_result(const char *id_tag, auth_result_t result)
{
    if (id_tag && s_pending_tag[0] && strcmp(id_tag, s_pending_tag) != 0) {
        return;                     /* result for a tag we are not waiting on */
    }
    s_auth_state = result;
    hmi_play(result == AUTH_ACCEPTED ? HMI_TONE_AUTHORISED : HMI_TONE_REJECTED);
}

bool evse_sm_request_stop(const char *id_tag, stop_reason_t reason)
{
    if (!s_txn.active) return false;

    /*
     * A tag-initiated stop must come from the tag that started the session.
     * Without this check any tag in the car park can stop anyone's charge.
     * Remote and system stops pass NULL and bypass it deliberately.
     */
    if (id_tag != NULL && strcmp(id_tag, s_txn.id_tag) != 0) {
        hmi_play(HMI_TONE_REJECTED);
        return false;
    }

    stop_supply(reason);
    enter_state(EVSE_STATE_FINISHING);
    return true;
}

void evse_sm_set_profile_limit(float amps)
{
    s_profile_limit_a = amps;
}

void evse_sm_set_available(bool available)
{
    s_available = available;
}

bool evse_sm_reserve(int32_t reservation_id, const char *id_tag, uint32_t expiry)
{
    if (s_st.state != EVSE_STATE_IDLE) return false;
    s_reservation_id = reservation_id;
    s_reservation_expiry = expiry;
    snprintf(s_reservation_tag, sizeof(s_reservation_tag), "%s", id_tag ? id_tag : "");
    enter_state(EVSE_STATE_RESERVED);
    return true;
}

void evse_sm_cancel_reservation(int32_t reservation_id)
{
    if (s_reservation_id != reservation_id) return;
    s_reservation_id = -1;
    s_reservation_tag[0] = '\0';
    if (s_st.state == EVSE_STATE_RESERVED) {
        enter_state(EVSE_STATE_IDLE);
    }
}

uint32_t evse_sm_try_clear_fault(void)
{
    uint32_t remaining = safety_clear_recoverable();
    s_st.faults = remaining;
    if ((remaining & EVSE_FAULT_LATCHING_MASK) == 0u &&
        s_st.state == EVSE_STATE_FAULTED && remaining == 0u) {
        enter_state(EVSE_STATE_IDLE);
    }
    return remaining;
}

bool evse_sm_transaction_active(void) { return s_txn.active; }

void evse_sm_set_transaction_id(uint32_t local_id, int32_t transaction_id)
{
    if (s_txn.local_id == local_id) {
        s_txn.transaction_id = transaction_id;
    }
}

void evse_sm_get_status(evse_status_t *out)
{
    if (!out) return;
    *out = s_st;
    out->txn = s_txn;
}

/* ---------------------------------------------------------------------- */
/* The state machine                                                      */
/* ---------------------------------------------------------------------- */

void evse_sm_update(void)
{
    cp_status_t cp;
    cp_pilot_get(&cp);
    proximity_update();

    meter_reading_t m;
    meter_get(&m);

    s_st.pilot   = cp;
    s_st.meter   = m;
    s_st.faults  = safety_get_faults();
    s_st.uptime_s = now_ms() / 1000u;
    safety_get_temps(&s_st.temp_connector_c, &s_st.temp_internal_c);

    /* ---- Pilot-derived faults the supervisor cannot see on its own ---- */
    if (cp.state == CP_STATE_INVALID) {
        s_st.faults |= EVSE_FAULT_CP_INVALID;
    }
    if (cp.state == CP_STATE_E) {
        s_st.faults |= EVSE_FAULT_CP_SHORT;
    }
    /*
     * The diode check is only meaningful while modulating, and only matters
     * once the vehicle claims to be ready. A resistor-only load — a damaged
     * cable or a deliberate bypass — decodes as a perfectly good state C, and
     * this is the only thing that distinguishes it from a real vehicle.
     */
    if (cp.pwm_active && !cp.diode_ok &&
        (cp.state == CP_STATE_C || cp.state == CP_STATE_D)) {
        s_st.faults |= EVSE_FAULT_CP_DIODE;
    }
    if (cp.state == CP_STATE_D && !cfg_get()->ventilation_available) {
        s_st.faults |= EVSE_FAULT_VENT_REQUIRED;
    }

    /* ---- Global overrides ------------------------------------------- */

    if (fault_blocking(s_st.faults)) {
        if (relay_is_closed()) {
            relay_emergency_open();
        }
        if (s_txn.active) {
            stop_supply((s_st.faults & EVSE_FAULT_ESTOP)
                        ? STOP_REASON_EMERGENCY_STOP : STOP_REASON_OTHER);
        }
        cp_pilot_set_static(CP_STATE_F);

        if (s_st.faults & EVSE_FAULT_LATCHING_MASK) {
            /* Persist so the lockout survives the power cycle it invites. */
            if (cfg_get()->lockout_faults != (s_st.faults & EVSE_FAULT_LATCHING_MASK)) {
                cfg_mutable()->lockout_faults = s_st.faults & EVSE_FAULT_LATCHING_MASK;
                (void)cfg_save();
            }
            enter_state(EVSE_STATE_LOCKOUT);
        } else {
            enter_state(EVSE_STATE_FAULTED);
        }
        s_st.offered_a = 0.0f;
        return;
    }

    /* ---- Per-state behaviour ----------------------------------------- */

    switch (s_st.state) {

    case EVSE_STATE_BOOT:
        /* main() runs the self-test and moves us on; nothing to do here. */
        break;

    case EVSE_STATE_LOCKOUT:
        /* Only a service action clears this. Keep the pilot at state F. */
        cp_pilot_set_static(CP_STATE_F);
        s_st.offered_a = 0.0f;
        break;

    case EVSE_STATE_FAULTED:
        cp_pilot_set_static(CP_STATE_F);
        s_st.offered_a = 0.0f;
        /*
         * Recover only once the cable is out. Re-arming with the vehicle still
         * connected would re-energise into whatever caused the fault.
         */
        if (s_st.faults == 0u && cp.state == CP_STATE_A) {
            cp_pilot_set_static(CP_STATE_A);
            enter_state(EVSE_STATE_IDLE);
        }
        break;

    case EVSE_STATE_UNAVAILABLE:
        cp_pilot_set_static(CP_STATE_F);
        s_st.offered_a = 0.0f;
        if (s_available) {
            cp_pilot_set_static(CP_STATE_A);
            enter_state(EVSE_STATE_IDLE);
        }
        break;

    case EVSE_STATE_IDLE:
        cp_pilot_set_static(CP_STATE_A);
        s_st.offered_a = 0.0f;
        if (!s_available) {
            enter_state(EVSE_STATE_UNAVAILABLE);
        } else if (cp.state == CP_STATE_B) {
            hmi_play(HMI_TONE_PLUG_IN);
            enter_state(EVSE_STATE_CONNECTED);
        }
        break;

    case EVSE_STATE_RESERVED:
        cp_pilot_set_static(CP_STATE_A);
        s_st.offered_a = 0.0f;
        if (rtc_unix_time() >= s_reservation_expiry) {
            s_reservation_id = -1;
            enter_state(EVSE_STATE_IDLE);
        } else if (cp.state == CP_STATE_B) {
            /* Only the holder's tag may take a reserved connector. */
            enter_state(EVSE_STATE_CONNECTED);
        }
        break;

    case EVSE_STATE_CONNECTED: {
        /*
         * Cable in, not yet authorised. Hold a static +12 V rather than
         * starting PWM: a duty cycle is an invitation to draw current, and we
         * have not decided whether this session may.
         */
        cp_pilot_set_static(CP_STATE_A);
        s_st.offered_a = 0.0f;

        if (cp.state == CP_STATE_A) {
            s_pending_tag[0] = '\0';
            enter_state(EVSE_STATE_IDLE);
            break;
        }

        bool authorised = cfg_get()->free_vend || (s_auth_state == AUTH_ACCEPTED);
        if (authorised) {
            const char *tag = cfg_get()->free_vend && s_pending_tag[0] == '\0'
                            ? "FREEVEND" : s_pending_tag;
            snprintf(s_active_tag, sizeof(s_active_tag), "%s", tag);
            start_transaction(s_active_tag);
            s_pending_tag[0] = '\0';
            s_auth_state = AUTH_PENDING;
            enter_state(EVSE_STATE_PREPARING);
        } else if (in_state_ms() > CONNECT_TIMEOUT_MS) {
            s_pending_tag[0] = '\0';
            enter_state(EVSE_STATE_FINISHING);
        }
        break;
    }

    case EVSE_STATE_PREPARING: {
        /* Authorised: advertise current and wait for the vehicle to accept. */
        float offer = compute_offer();
        s_st.offered_a = offer;

        if (offer < EVSE_MIN_CURRENT_A) {
            cp_pilot_set_static(CP_STATE_A);
            enter_state(EVSE_STATE_SUSPENDED_EVSE);
            break;
        }
        cp_pilot_set_offer(offer);

        if (cp.state == CP_STATE_A || cp.state == CP_STATE_B) {
            if (cp.state == CP_STATE_A) {
                stop_supply(STOP_REASON_EV_DISCONNECTED);
                enter_state(EVSE_STATE_IDLE);
            }
            break;                    /* still waiting in B */
        }

        if (vehicle_requests_charge(&cp) && in_state_ms() >= PRECLOSE_SETTLE_MS) {
            relay_result_t r = relay_close();
            if (r == RELAY_OK) {
                s_zero_current_since_ms = 0;
                hmi_play(HMI_TONE_CHARGE_START);
                enter_state(EVSE_STATE_CHARGING);
            } else if (r == RELAY_ERR_WELDED) {
                /* safety.c will latch this on its next pass. */
                enter_state(EVSE_STATE_FAULTED);
            } else if (++s_close_retries > CLOSE_RETRY_LIMIT) {
                relay_inhibit();
                enter_state(EVSE_STATE_LOCKOUT);
            }
        }
        break;
    }

    case EVSE_STATE_CHARGING: {
        float offer = compute_offer();
        s_st.offered_a = offer;

        /* Vehicle withdrew its request, or the cable came out. */
        if (!vehicle_requests_charge(&cp)) {
            (void)relay_open();
            if (cp.state == CP_STATE_A) {
                stop_supply(STOP_REASON_EV_DISCONNECTED);
                enter_state(EVSE_STATE_FINISHING);
            } else {
                s_zero_current_since_ms = now_ms();
                enter_state(EVSE_STATE_SUSPENDED_EV);
            }
            break;
        }

        if (offer < EVSE_MIN_CURRENT_A) {
            (void)relay_open();
            cp_pilot_set_static(CP_STATE_A);
            enter_state(EVSE_STATE_SUSPENDED_EVSE);
            break;
        }
        cp_pilot_set_offer(offer);

        /*
         * A vehicle in state C that draws nothing for a minute has finished but
         * has not dropped the pilot. Track it so the session reports as
         * suspended rather than charging, without opening the contactor — some
         * vehicles resume after a balancing pause.
         */
        if (m.valid && m.current_a < 0.5f) {
            if (s_zero_current_since_ms == 0u) {
                s_zero_current_since_ms = now_ms();
            } else if (now_ms() - s_zero_current_since_ms > EV_SUSPEND_MS) {
                enter_state(EVSE_STATE_SUSPENDED_EV);
            }
        } else {
            s_zero_current_since_ms = 0u;
        }
        break;
    }

    case EVSE_STATE_SUSPENDED_EV:
        s_st.offered_a = compute_offer();
        if (cp.state == CP_STATE_A) {
            stop_supply(STOP_REASON_EV_DISCONNECTED);
            enter_state(EVSE_STATE_FINISHING);
        } else if (vehicle_requests_charge(&cp) && !relay_is_closed()) {
            if (relay_close() == RELAY_OK) {
                s_zero_current_since_ms = 0u;
                enter_state(EVSE_STATE_CHARGING);
            }
        } else if (relay_is_closed() && m.valid && m.current_a > 1.0f) {
            s_zero_current_since_ms = 0u;
            enter_state(EVSE_STATE_CHARGING);
        }
        break;

    case EVSE_STATE_SUSPENDED_EVSE: {
        float offer = compute_offer();
        s_st.offered_a = offer;
        if (cp.state == CP_STATE_A) {
            stop_supply(STOP_REASON_EV_DISCONNECTED);
            enter_state(EVSE_STATE_FINISHING);
        } else if (offer >= EVSE_MIN_CURRENT_A) {
            enter_state(EVSE_STATE_PREPARING);
        }
        break;
    }

    case EVSE_STATE_FINISHING:
        cp_pilot_set_static(CP_STATE_A);
        s_st.offered_a = 0.0f;
        if (relay_is_closed()) {
            (void)relay_open();
        }
        if (cp.state == CP_STATE_A) {
            /* Prove the contactor really opened before offering the next car. */
            if (relay_check_weld() == RELAY_OK) {
                enter_state(s_available ? EVSE_STATE_IDLE : EVSE_STATE_UNAVAILABLE);
            }
        }
        break;
    }
}
