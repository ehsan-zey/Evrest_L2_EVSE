/**
 * @file  ocpp_profile.h
 * @brief OCPP 1.6 SmartCharging profile storage and composite schedule.
 *
 * A charge point can hold several profiles at once and has to work out, at any
 * instant, what the resulting current limit is. OCPP 1.6 §3.13 defines the
 * precedence:
 *
 *   1. **ChargePointMaxProfile** is an absolute ceiling on the whole charge
 *      point. It never raises a limit, only caps whatever the others produce.
 *   2. **TxProfile** applies to the running transaction and beats
 *      TxDefaultProfile.
 *   3. **TxDefaultProfile** is the fallback when no TxProfile exists.
 *
 * Within one purpose the highest `stackLevel` wins outright — profiles at
 * different stack levels do not blend.
 *
 * All of this is pure computation over stored state, so it is unit-tested on
 * the host; getting it wrong means a site's load management silently does
 * nothing, which is not visible until the main breaker trips.
 */
#ifndef OCPP_PROFILE_H
#define OCPP_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define OCPP_MAX_PROFILES        8
#define OCPP_MAX_PERIODS        12

typedef enum {
    OCPP_PURPOSE_CHARGE_POINT_MAX = 0,
    OCPP_PURPOSE_TX_DEFAULT,
    OCPP_PURPOSE_TX,
    OCPP_PURPOSE_INVALID
} ocpp_purpose_t;

typedef enum {
    OCPP_KIND_ABSOLUTE = 0,
    OCPP_KIND_RECURRING,
    OCPP_KIND_RELATIVE
} ocpp_kind_t;

typedef enum {
    OCPP_RECUR_NONE = 0,
    OCPP_RECUR_DAILY,
    OCPP_RECUR_WEEKLY
} ocpp_recurrency_t;

typedef enum {
    OCPP_RATE_AMPS = 0,
    OCPP_RATE_WATTS
} ocpp_rate_unit_t;

typedef struct {
    int32_t start_period;   /**< seconds from the schedule start */
    float   limit;          /**< amps or watts, per rate_unit    */
    int32_t phases;         /**< 0 if unspecified                */
} ocpp_period_t;

typedef struct {
    bool              valid;
    int32_t           profile_id;
    int32_t           transaction_id;   /**< TxProfile only; -1 otherwise */
    int32_t           stack_level;
    ocpp_purpose_t    purpose;
    ocpp_kind_t       kind;
    ocpp_recurrency_t recurrency;
    ocpp_rate_unit_t  rate_unit;

    uint32_t valid_from;       /**< Unix seconds, 0 = always            */
    uint32_t valid_to;         /**< Unix seconds, 0 = never expires     */
    uint32_t start_schedule;   /**< Unix seconds; anchor for Absolute   */
    int32_t  duration_s;       /**< 0 = until the next period ends      */
    float    min_charging_rate;

    ocpp_period_t periods[OCPP_MAX_PERIODS];
    size_t        period_count;
} ocpp_profile_t;

void ocpp_profile_init(void);

/**
 * Install or replace a profile. A profile with the same id, or the same
 * purpose *and* stack level, replaces the existing one — that is what OCPP
 * 1.6 §3.13.2 requires, and it is how a CSMS updates a limit without first
 * clearing it.
 * @return false if there is no room.
 */
bool ocpp_profile_set(const ocpp_profile_t *p);

/**
 * Clear profiles matching the given filters. Any filter may be "unset":
 * pass a negative profile_id / stack_level, or OCPP_PURPOSE_INVALID.
 * @return number of profiles cleared.
 */
int ocpp_profile_clear(int32_t profile_id, ocpp_purpose_t purpose, int32_t stack_level);

/** Drop every TxProfile — done automatically when a transaction ends. */
void ocpp_profile_clear_tx(void);

/**
 * The composite current limit at @p now.
 *
 * @param now              Unix seconds
 * @param txn_start        Unix seconds the running transaction began, or 0
 * @param nominal_voltage  used to convert W-based profiles to amps
 * @param fallback_a       returned when no profile applies
 */
float ocpp_profile_limit_a(uint32_t now, uint32_t txn_start,
                           float nominal_voltage, uint8_t phases,
                           float fallback_a);

/**
 * Evaluate one profile at @p now.
 * @return true if the profile is in effect, with *limit set in its own units.
 */
bool ocpp_profile_eval(const ocpp_profile_t *p, uint32_t now, uint32_t txn_start,
                       float *limit);

/** Number of profiles currently stored. */
size_t ocpp_profile_count(void);

/** Read back a stored profile for GetCompositeSchedule and diagnostics. */
const ocpp_profile_t *ocpp_profile_at(size_t index);

#endif /* OCPP_PROFILE_H */
