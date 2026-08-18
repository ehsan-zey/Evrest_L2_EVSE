/**
 * @file  evse_types.h
 * @brief Types shared across the EVSE application layer.
 */
#ifndef EVSE_TYPES_H
#define EVSE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ======================================================================== */
/* Control pilot                                                            */
/* ======================================================================== */

/** IEC 61851-1 Annex A pilot states, decoded from the CP high plateau. */
typedef enum {
    CP_STATE_A = 0,   /**< +12 V — no vehicle connected                     */
    CP_STATE_B,       /**< +9 V  — vehicle connected, not ready             */
    CP_STATE_C,       /**< +6 V  — vehicle ready, no ventilation required   */
    CP_STATE_D,       /**< +3 V  — vehicle ready, ventilation required      */
    CP_STATE_E,       /**< 0 V   — error: CP shorted to PE, or EVSE fault   */
    CP_STATE_F,       /**< -12 V — EVSE unavailable                         */
    CP_STATE_INVALID  /**< reading outside every defined window             */
} cp_state_t;

/** Snapshot of the pilot line as measured by the pilot task. */
typedef struct {
    cp_state_t state;          /**< debounced state                          */
    cp_state_t raw_state;      /**< most recent undebounced decode           */
    int16_t    v_high_mv;      /**< CP positive plateau, millivolts          */
    int16_t    v_low_mv;       /**< CP negative plateau, millivolts          */
    float      duty_pct;       /**< duty cycle we are currently driving      */
    float      offered_a;      /**< current advertised by that duty cycle    */
    bool       diode_ok;       /**< vehicle diode present (low plateau < -10V)*/
    bool       pwm_active;     /**< true when modulating, false when static  */
    uint32_t   state_since_ms; /**< tick at which `state` was entered        */
} cp_status_t;

/* ======================================================================== */
/* Faults                                                                   */
/* ======================================================================== */

/**
 * Fault bits. Everything the safety supervisor can detect has a bit here so a
 * single uint32 carries the complete fault picture to the state machine, the
 * HMI and OCPP.
 *
 * "Latching" faults survive the condition clearing and require a power cycle or
 * an explicit authenticated reset — see EVSE_FAULT_LATCHING_MASK.
 */
typedef enum {
    EVSE_FAULT_NONE           = 0u,
    EVSE_FAULT_RCD_TRIP       = 1u << 0,  /**< residual current detected     */
    EVSE_FAULT_RCD_SELFTEST   = 1u << 1,  /**< RCD failed its self-test      */
    EVSE_FAULT_PEN            = 1u << 2,  /**< open-PEN / lost neutral       */
    EVSE_FAULT_PE_LOST        = 1u << 3,  /**< protective earth not present  */
    EVSE_FAULT_RELAY_WELD     = 1u << 4,  /**< contact welded closed         */
    EVSE_FAULT_RELAY_NO_CLOSE = 1u << 5,  /**< commanded closed, no current  */
    EVSE_FAULT_ESTOP          = 1u << 6,  /**< emergency-off asserted        */
    EVSE_FAULT_OVER_TEMP      = 1u << 7,  /**< thermal trip                  */
    EVSE_FAULT_OVER_CURRENT   = 1u << 8,  /**< current beyond the offer      */
    EVSE_FAULT_CP_INVALID     = 1u << 9,  /**< pilot outside all windows     */
    EVSE_FAULT_CP_DIODE       = 1u << 10, /**< vehicle diode missing/shorted */
    EVSE_FAULT_CP_SHORT       = 1u << 11, /**< state E: CP shorted to PE     */
    EVSE_FAULT_VENT_REQUIRED  = 1u << 12, /**< state D but site has no vent  */
    EVSE_FAULT_METER_COMM     = 1u << 13, /**< metering IC unreachable       */
    EVSE_FAULT_OVER_VOLTAGE   = 1u << 14,
    EVSE_FAULT_UNDER_VOLTAGE  = 1u << 15,
    EVSE_FAULT_SELFTEST       = 1u << 16, /**< power-on self-test failed     */
} evse_fault_t;

/**
 * Faults that must not be cleared automatically. A welded contactor or a failed
 * RCD self-test means a protective element is gone; re-arming on our own would
 * mean energising the cable with no protection behind it.
 */
#define EVSE_FAULT_LATCHING_MASK  (EVSE_FAULT_RELAY_WELD    | \
                                   EVSE_FAULT_RCD_SELFTEST  | \
                                   EVSE_FAULT_SELFTEST      | \
                                   EVSE_FAULT_RELAY_NO_CLOSE)

/** Faults that require the contactor open immediately, no ramp-down. */
#define EVSE_FAULT_TRIP_MASK      (EVSE_FAULT_RCD_TRIP  | EVSE_FAULT_PEN       | \
                                   EVSE_FAULT_PE_LOST   | EVSE_FAULT_ESTOP     | \
                                   EVSE_FAULT_OVER_TEMP | EVSE_FAULT_CP_SHORT  | \
                                   EVSE_FAULT_RELAY_WELD| EVSE_FAULT_OVER_CURRENT)

/* ======================================================================== */
/* Charging state machine                                                   */
/* ======================================================================== */

typedef enum {
    EVSE_STATE_BOOT = 0,     /**< running power-on self-test               */
    EVSE_STATE_IDLE,         /**< available, nothing plugged in            */
    EVSE_STATE_CONNECTED,    /**< cable in, waiting for authorisation      */
    EVSE_STATE_PREPARING,    /**< authorised, PWM on, waiting for state C  */
    EVSE_STATE_CHARGING,     /**< contactor closed, energy flowing         */
    EVSE_STATE_SUSPENDED_EV, /**< vehicle stopped drawing (state B w/ txn)  */
    EVSE_STATE_SUSPENDED_EVSE,/**< we withdrew the offer (profile, derate)  */
    EVSE_STATE_FINISHING,    /**< transaction closed, waiting for unplug   */
    EVSE_STATE_RESERVED,     /**< held by an OCPP reservation              */
    EVSE_STATE_UNAVAILABLE,  /**< operative=false via ChangeAvailability   */
    EVSE_STATE_FAULTED,      /**< recoverable fault present                */
    EVSE_STATE_LOCKOUT       /**< latching fault; needs service            */
} evse_state_t;

/** Why the current transaction ended — maps onto OCPP `Reason`. */
typedef enum {
    STOP_REASON_LOCAL = 0,
    STOP_REASON_REMOTE,
    STOP_REASON_EV_DISCONNECTED,
    STOP_REASON_EMERGENCY_STOP,
    STOP_REASON_POWER_LOSS,
    STOP_REASON_REBOOT,
    STOP_REASON_DE_AUTHORIZED,
    STOP_REASON_OTHER
} stop_reason_t;

/* ======================================================================== */
/* Metering                                                                 */
/* ======================================================================== */

typedef struct {
    float    voltage_v;       /**< RMS line voltage                          */
    float    current_a;       /**< RMS line current                          */
    float    active_power_w;  /**< signed active power                       */
    float    power_factor;    /**< -1 .. 1                                   */
    float    frequency_hz;
    uint64_t energy_wh;       /**< lifetime import register, watt-hours      */
    bool     valid;           /**< false if the metering IC did not answer   */
    uint32_t updated_ms;
} meter_reading_t;

/* ======================================================================== */
/* Transaction                                                              */
/* ======================================================================== */

#define OCPP_IDTAG_MAXLEN   21   /* 20 chars + NUL, per OCPP 1.6 CiString20 */

typedef struct {
    int32_t  transaction_id;      /**< assigned by the CSMS; <0 until then   */
    uint32_t local_id;            /**< our own id, valid while offline       */
    char     id_tag[OCPP_IDTAG_MAXLEN];
    uint64_t meter_start_wh;
    uint64_t meter_stop_wh;
    uint32_t start_time;          /**< Unix seconds, UTC                     */
    uint32_t stop_time;
    stop_reason_t stop_reason;
    bool     active;
} evse_transaction_t;

/* ======================================================================== */
/* Aggregate status, published to HMI and OCPP                              */
/* ======================================================================== */

typedef struct {
    evse_state_t       state;
    uint32_t           faults;         /**< bitwise OR of evse_fault_t       */
    cp_status_t        pilot;
    meter_reading_t    meter;
    evse_transaction_t txn;
    float              offered_a;      /**< what we advertise to the vehicle */
    float              limit_hw_a;     /**< SKU rating                       */
    float              limit_install_a;/**< electrician setting from NVM     */
    float              limit_profile_a;/**< OCPP smart-charging limit        */
    float              limit_thermal_a;/**< thermal derate                   */
    int16_t            temp_connector_c;
    int16_t            temp_internal_c;
    bool               network_up;
    bool               csms_connected;
    uint32_t           uptime_s;
} evse_status_t;

/* Small helpers used all over the app. */
static inline float evse_minf(float a, float b) { return (a < b) ? a : b; }
static inline float evse_maxf(float a, float b) { return (a > b) ? a : b; }
static inline float evse_clampf(float v, float lo, float hi) {
    return evse_minf(evse_maxf(v, lo), hi);
}

const char *evse_state_name(evse_state_t s);
const char *cp_state_name(cp_state_t s);
/** Human-readable name of the most significant set fault bit, or "None". */
const char *evse_fault_name(uint32_t faults);

#endif /* EVSE_TYPES_H */
