/**
 * @file  ocpp_auth.c
 * @brief Local authorisation list and cache. See ocpp_auth.h.
 */
#include "ocpp_auth.h"
#include <string.h>
#include <stdio.h>

static ocpp_tag_entry_t s_list[OCPP_AUTH_LIST_MAX];
static ocpp_tag_entry_t s_cache[OCPP_AUTH_CACHE_MAX];
static int32_t          s_list_version;

/* Staging for an in-progress SendLocalList: the live list is only replaced on
 * commit, so a partial or failed update cannot leave it half-written. */
static ocpp_tag_entry_t s_staging[OCPP_AUTH_LIST_MAX];
static int32_t          s_staging_version;
static bool             s_update_active;

void ocpp_auth_init(void)
{
    memset(s_list, 0, sizeof(s_list));
    memset(s_cache, 0, sizeof(s_cache));
    s_list_version = 0;
    s_update_active = false;
}

int32_t ocpp_auth_list_version(void) { return s_list_version; }

size_t ocpp_auth_list_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < OCPP_AUTH_LIST_MAX; i++) if (s_list[i].in_use) n++;
    return n;
}

void ocpp_auth_list_begin(int32_t version, bool full)
{
    if (full) {
        memset(s_staging, 0, sizeof(s_staging));
    } else {
        memcpy(s_staging, s_list, sizeof(s_staging));
    }
    s_staging_version = version;
    s_update_active = true;
}

bool ocpp_auth_list_put(const ocpp_tag_entry_t *entry, bool remove)
{
    if (!s_update_active || entry == NULL) return false;

    int free_slot = -1;
    for (int i = 0; i < OCPP_AUTH_LIST_MAX; i++) {
        if (!s_staging[i].in_use) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(s_staging[i].id_tag, entry->id_tag) == 0) {
            if (remove) {
                s_staging[i].in_use = false;
            } else {
                s_staging[i] = *entry;
                s_staging[i].in_use = true;
            }
            return true;
        }
    }

    if (remove) return true;              /* removing something absent is fine */
    if (free_slot < 0) return false;      /* list full */

    s_staging[free_slot] = *entry;
    s_staging[free_slot].in_use = true;
    return true;
}

void ocpp_auth_list_commit(void)
{
    if (!s_update_active) return;
    memcpy(s_list, s_staging, sizeof(s_list));
    s_list_version = s_staging_version;
    s_update_active = false;
}

void ocpp_auth_list_abort(void) { s_update_active = false; }

void ocpp_auth_cache_put(const char *id_tag, const char *parent_id,
                         ocpp_tag_status_t status, uint32_t expiry)
{
    if (id_tag == NULL || id_tag[0] == '\0') return;

    int slot = -1;
    for (int i = 0; i < OCPP_AUTH_CACHE_MAX; i++) {
        if (s_cache[i].in_use && strcmp(s_cache[i].id_tag, id_tag) == 0) {
            slot = i;
            break;
        }
        if (!s_cache[i].in_use && slot < 0) slot = i;
    }
    /*
     * If the cache is full, overwrite slot 0 rather than dropping the result.
     * A proper LRU would be better, but the cache is a convenience and the
     * cost of being wrong is one extra Authorize round trip.
     */
    if (slot < 0) slot = 0;

    memset(&s_cache[slot], 0, sizeof(s_cache[slot]));
    snprintf(s_cache[slot].id_tag, sizeof(s_cache[slot].id_tag), "%s", id_tag);
    if (parent_id) {
        snprintf(s_cache[slot].parent_id, sizeof(s_cache[slot].parent_id), "%s", parent_id);
    }
    s_cache[slot].status = status;
    s_cache[slot].expiry = expiry;
    s_cache[slot].in_use = true;
}

void ocpp_auth_cache_clear(void)
{
    memset(s_cache, 0, sizeof(s_cache));
}

/** Look a tag up in one table. */
static const ocpp_tag_entry_t *find_in(const ocpp_tag_entry_t *table, size_t n,
                                       const char *id_tag)
{
    for (size_t i = 0; i < n; i++) {
        if (table[i].in_use && strcmp(table[i].id_tag, id_tag) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

bool ocpp_auth_lookup(const char *id_tag, uint32_t now, bool allow_cache,
                      ocpp_tag_entry_t *out)
{
    if (id_tag == NULL || id_tag[0] == '\0') return false;

    /*
     * Local list first, and its verdict is final either way. Falling through
     * to the cache on a Blocked entry would let a revoked card keep working
     * from a stale accept, which is exactly what the local list exists to
     * prevent.
     */
    const ocpp_tag_entry_t *e = find_in(s_list, OCPP_AUTH_LIST_MAX, id_tag);
    if (e != NULL) {
        if (out) {
            *out = *e;
            if (e->status == OCPP_TAG_ACCEPTED &&
                e->expiry != 0u && now != 0u && now > e->expiry) {
                out->status = OCPP_TAG_EXPIRED;
            }
        }
        return true;
    }

    if (!allow_cache) return false;

    e = find_in(s_cache, OCPP_AUTH_CACHE_MAX, id_tag);
    if (e == NULL) return false;

    /* An expired cache entry is not a decision — it means ask again. */
    if (e->expiry != 0u && now != 0u && now > e->expiry) {
        return false;
    }
    if (out) *out = *e;
    return true;
}

bool ocpp_auth_same_group(const char *starter, const char *candidate)
{
    if (starter == NULL || candidate == NULL) return false;
    if (strcmp(starter, candidate) == 0) return true;

    ocpp_tag_entry_t a, b;
    if (!ocpp_auth_lookup(starter, 0, true, &a))   return false;
    if (!ocpp_auth_lookup(candidate, 0, true, &b)) return false;

    /*
     * An empty parentIdTag means "no group". Two tags that both lack one are
     * not in the same group, so the emptiness check has to come first — string
     * comparison alone would make every ungrouped tag able to stop every other
     * ungrouped tag's session.
     */
    if (a.parent_id[0] == '\0' || b.parent_id[0] == '\0') return false;
    return strcmp(a.parent_id, b.parent_id) == 0;
}

ocpp_tag_status_t ocpp_auth_status_from_string(const char *s)
{
    if (s == NULL)                          return OCPP_TAG_INVALID;
    if (strcmp(s, "Accepted") == 0)         return OCPP_TAG_ACCEPTED;
    if (strcmp(s, "Blocked") == 0)          return OCPP_TAG_BLOCKED;
    if (strcmp(s, "Expired") == 0)          return OCPP_TAG_EXPIRED;
    if (strcmp(s, "ConcurrentTx") == 0)     return OCPP_TAG_CONCURRENT_TX;
    return OCPP_TAG_INVALID;
}

const char *ocpp_auth_status_to_string(ocpp_tag_status_t s)
{
    switch (s) {
        case OCPP_TAG_ACCEPTED:      return "Accepted";
        case OCPP_TAG_BLOCKED:       return "Blocked";
        case OCPP_TAG_EXPIRED:       return "Expired";
        case OCPP_TAG_CONCURRENT_TX: return "ConcurrentTx";
        case OCPP_TAG_INVALID:       break;
    }
    return "Invalid";
}

auth_result_t ocpp_auth_to_result(ocpp_tag_status_t s)
{
    switch (s) {
        case OCPP_TAG_ACCEPTED: return AUTH_ACCEPTED;
        case OCPP_TAG_EXPIRED:  return AUTH_EXPIRED;
        case OCPP_TAG_BLOCKED:  return AUTH_BLOCKED;
        default:                return AUTH_REJECTED;
    }
}
