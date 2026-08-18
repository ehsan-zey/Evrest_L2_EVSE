/**
 * @file  ocpp_auth.h
 * @brief Local authorisation list and authorisation cache.
 *
 * Two distinct things that OCPP 1.6 keeps separate and which behave
 * differently when the charger is offline:
 *
 * **Local list** (`SendLocalList`) is pushed by the CSMS and is authoritative.
 * A tag in it can be charged without asking anyone. It is versioned so the
 * CSMS can tell whether the charger is up to date.
 *
 * **Cache** is what the charger remembers from previous `Authorize` responses.
 * It is a convenience, not an authority: entries expire, and whether it may be
 * used at all when offline is a configuration decision, because trusting it
 * unconditionally means a revoked card keeps working until it expires.
 *
 * Lookup order is local list, then cache, and a `Blocked` or `Expired` entry in
 * the local list is honoured — a deny in the authoritative list must not fall
 * through to an older cached accept.
 */
#ifndef OCPP_AUTH_H
#define OCPP_AUTH_H

#include "evse_types.h"
#include "evse_sm.h"
#include <stddef.h>

#define OCPP_AUTH_LIST_MAX    64
#define OCPP_AUTH_CACHE_MAX   32

typedef enum {
    OCPP_TAG_ACCEPTED = 0,
    OCPP_TAG_BLOCKED,
    OCPP_TAG_EXPIRED,
    OCPP_TAG_INVALID,
    OCPP_TAG_CONCURRENT_TX
} ocpp_tag_status_t;

typedef struct {
    char              id_tag[OCPP_IDTAG_MAXLEN];
    char              parent_id[OCPP_IDTAG_MAXLEN];
    ocpp_tag_status_t status;
    uint32_t          expiry;      /**< Unix seconds; 0 = no expiry */
    bool              in_use;
} ocpp_tag_entry_t;

void ocpp_auth_init(void);

/* ---- Local list ---- */

int32_t ocpp_auth_list_version(void);

/**
 * Begin a SendLocalList update.
 * @param full  true for a Full update, which clears the list first
 */
void ocpp_auth_list_begin(int32_t version, bool full);

/**
 * Add or remove one entry during an update. An entry with no status field in
 * the request means "delete this tag", which is how a Differential update
 * revokes a card.
 */
bool ocpp_auth_list_put(const ocpp_tag_entry_t *entry, bool remove);

/** Commit the update, publishing the new version. */
void ocpp_auth_list_commit(void);

/** Abort an in-progress update, leaving the previous list intact. */
void ocpp_auth_list_abort(void);

size_t ocpp_auth_list_count(void);

/* ---- Cache ---- */

/** Record an Authorize result. */
void ocpp_auth_cache_put(const char *id_tag, const char *parent_id,
                         ocpp_tag_status_t status, uint32_t expiry);

/** ClearCache. The local list is untouched. */
void ocpp_auth_cache_clear(void);

/* ---- Lookup ---- */

/**
 * Resolve a tag locally.
 *
 * @param now             Unix seconds, for expiry checks
 * @param allow_cache     may the cache satisfy this lookup (offline policy)
 * @param out             the matched entry, if any
 * @return true if a local decision was reached; false means "ask the CSMS".
 */
bool ocpp_auth_lookup(const char *id_tag, uint32_t now, bool allow_cache,
                      ocpp_tag_entry_t *out);

/**
 * True if @p candidate may stop a transaction started by @p starter — the same
 * tag, or a tag sharing the same non-empty parentIdTag.
 */
bool ocpp_auth_same_group(const char *starter, const char *candidate);

/** Map an OCPP idTagInfo status string onto the enum. */
ocpp_tag_status_t ocpp_auth_status_from_string(const char *s);
const char *ocpp_auth_status_to_string(ocpp_tag_status_t s);

/** Convert a tag status into the state machine's auth result. */
auth_result_t ocpp_auth_to_result(ocpp_tag_status_t s);

#endif /* OCPP_AUTH_H */
