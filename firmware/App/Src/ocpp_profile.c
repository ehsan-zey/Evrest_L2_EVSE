/**
 * @file  ocpp_profile.c
 * @brief SmartCharging profile evaluation. See ocpp_profile.h.
 */
#include "ocpp_profile.h"
#include <string.h>

static ocpp_profile_t s_profiles[OCPP_MAX_PROFILES];

void ocpp_profile_init(void)
{
    memset(s_profiles, 0, sizeof(s_profiles));
}

size_t ocpp_profile_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < OCPP_MAX_PROFILES; i++) if (s_profiles[i].valid) n++;
    return n;
}

const ocpp_profile_t *ocpp_profile_at(size_t index)
{
    return (index < OCPP_MAX_PROFILES && s_profiles[index].valid)
         ? &s_profiles[index] : NULL;
}

bool ocpp_profile_set(const ocpp_profile_t *p)
{
    if (p == NULL || p->period_count == 0u) return false;

    /*
     * Replace on either match. Same id is the obvious case; same purpose and
     * stack level is the one that matters in practice, because a CSMS updating
     * a limit usually sends a new profile id at the same stack level and
     * expects it to supersede rather than stack.
     */
    int slot = -1;
    for (int i = 0; i < OCPP_MAX_PROFILES; i++) {
        if (!s_profiles[i].valid) { if (slot < 0) slot = i; continue; }
        if (s_profiles[i].profile_id == p->profile_id ||
            (s_profiles[i].purpose == p->purpose &&
             s_profiles[i].stack_level == p->stack_level)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return false;

    s_profiles[slot] = *p;
    s_profiles[slot].valid = true;
    return true;
}

int ocpp_profile_clear(int32_t profile_id, ocpp_purpose_t purpose, int32_t stack_level)
{
    int cleared = 0;
    for (int i = 0; i < OCPP_MAX_PROFILES; i++) {
        if (!s_profiles[i].valid) continue;
        if (profile_id  >= 0 && s_profiles[i].profile_id  != profile_id)  continue;
        if (stack_level >= 0 && s_profiles[i].stack_level != stack_level) continue;
        if (purpose != OCPP_PURPOSE_INVALID && s_profiles[i].purpose != purpose) continue;
        s_profiles[i].valid = false;
        cleared++;
    }
    return cleared;
}

void ocpp_profile_clear_tx(void)
{
    for (int i = 0; i < OCPP_MAX_PROFILES; i++) {
        if (s_profiles[i].valid && s_profiles[i].purpose == OCPP_PURPOSE_TX) {
            s_profiles[i].valid = false;
        }
    }
}

/**
 * Seconds elapsed into the schedule at @p now, or -1 if it is not running.
 *
 * Absolute  — measured from start_schedule.
 * Relative  — measured from the transaction start; without a transaction a
 *             relative profile has no anchor and does not apply.
 * Recurring — start_schedule is the anchor of the first period; the offset
 *             wraps modulo a day or a week.
 */
static int32_t schedule_offset(const ocpp_profile_t *p, uint32_t now, uint32_t txn_start)
{
    switch (p->kind) {

    case OCPP_KIND_RELATIVE:
        if (txn_start == 0u || now < txn_start) return -1;
        return (int32_t)(now - txn_start);

    case OCPP_KIND_ABSOLUTE:
        if (p->start_schedule == 0u) {
            /* No anchor given: treat it as having started immediately, which
             * is how most CSMS implementations use an Absolute profile with no
             * startSchedule. */
            return 0;
        }
        if (now < p->start_schedule) return -1;      /* not started yet */
        return (int32_t)(now - p->start_schedule);

    case OCPP_KIND_RECURRING: {
        if (p->start_schedule == 0u) return -1;
        uint32_t period = (p->recurrency == OCPP_RECUR_WEEKLY) ? 604800u : 86400u;
        if (p->recurrency == OCPP_RECUR_NONE) return -1;

        /*
         * The anchor may be in the past or the future; the modulo has to work
         * either way, so the difference is taken in signed arithmetic and
         * brought back into range rather than relying on unsigned wraparound.
         */
        int64_t delta = (int64_t)now - (int64_t)p->start_schedule;
        int64_t off = delta % (int64_t)period;
        if (off < 0) off += (int64_t)period;
        return (int32_t)off;
    }
    }
    return -1;
}

bool ocpp_profile_eval(const ocpp_profile_t *p, uint32_t now, uint32_t txn_start,
                       float *limit)
{
    if (p == NULL || !p->valid || p->period_count == 0u) return false;

    if (p->valid_from != 0u && now < p->valid_from) return false;
    if (p->valid_to   != 0u && now > p->valid_to)   return false;

    /* A TxProfile only applies while its transaction is running. */
    if (p->purpose == OCPP_PURPOSE_TX && txn_start == 0u) return false;

    int32_t offset = schedule_offset(p, now, txn_start);
    if (offset < 0) return false;

    /* duration 0 means "no explicit end"; a non-zero duration ends the
     * schedule, and for a recurring profile that leaves a gap each cycle. */
    if (p->duration_s > 0 && offset >= p->duration_s) return false;

    /*
     * Find the last period whose startPeriod is at or before the offset.
     * Periods are required to be ordered by startPeriod, but a CSMS that sends
     * them out of order should not produce a nonsense limit, so this scans all
     * of them and keeps the best match rather than stopping at the first.
     */
    const ocpp_period_t *best = NULL;
    for (size_t i = 0; i < p->period_count; i++) {
        const ocpp_period_t *pd = &p->periods[i];
        if (pd->start_period > offset) continue;
        if (best == NULL || pd->start_period > best->start_period) best = pd;
    }
    if (best == NULL) return false;      /* offset precedes every period */

    *limit = best->limit;
    return true;
}

/** Convert a profile's limit into amps. */
static float to_amps(const ocpp_profile_t *p, float limit,
                     float nominal_voltage, uint8_t phases)
{
    if (p->rate_unit != OCPP_RATE_WATTS) return limit;
    float denom = nominal_voltage * (float)(phases ? phases : 1u);
    return (denom > 0.0f) ? (limit / denom) : limit;
}

float ocpp_profile_limit_a(uint32_t now, uint32_t txn_start,
                           float nominal_voltage, uint8_t phases,
                           float fallback_a)
{
    const ocpp_profile_t *best_cp_max = NULL;
    const ocpp_profile_t *best_tx     = NULL;
    float cp_max_limit = 0.0f, tx_limit = 0.0f;

    for (size_t i = 0; i < OCPP_MAX_PROFILES; i++) {
        const ocpp_profile_t *p = &s_profiles[i];
        float limit;
        if (!ocpp_profile_eval(p, now, txn_start, &limit)) continue;

        if (p->purpose == OCPP_PURPOSE_CHARGE_POINT_MAX) {
            if (best_cp_max == NULL || p->stack_level > best_cp_max->stack_level) {
                best_cp_max = p;
                cp_max_limit = to_amps(p, limit, nominal_voltage, phases);
            }
            continue;
        }

        /*
         * TxProfile outranks TxDefaultProfile regardless of stack level; only
         * within the same purpose does stack level decide.
         */
        bool better;
        if (best_tx == NULL) {
            better = true;
        } else if (p->purpose != best_tx->purpose) {
            better = (p->purpose == OCPP_PURPOSE_TX);
        } else {
            better = (p->stack_level > best_tx->stack_level);
        }
        if (better) {
            best_tx = p;
            tx_limit = to_amps(p, limit, nominal_voltage, phases);
        }
    }

    float result = (best_tx != NULL) ? tx_limit : fallback_a;

    /* ChargePointMaxProfile is a ceiling, never a floor. */
    if (best_cp_max != NULL && cp_max_limit < result) {
        result = cp_max_limit;
    }
    return result;
}
