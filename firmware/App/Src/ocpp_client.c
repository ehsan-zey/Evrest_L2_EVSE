/**
 * @file  ocpp_client.c
 * @brief OCPP 1.6J client. See ocpp_client.h.
 */
#include "ocpp_client.h"
#include "ocpp_profile.h"
#include "ocpp_auth.h"
#include "ws_client.h"
#include "net_link.h"
#include "json.h"
#include "evse_board.h"
#include "evse_config.h"
#include "evse_sm.h"
#include "meter.h"
#include "safety.h"
#include "rtc_time.h"
#include "txn_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== */
/* Sizing                                                                 */
/* ====================================================================== */

#define OCPP_RX_BUF_SIZE     4096
#define OCPP_TX_BUF_SIZE     4096
#define OCPP_MSG_MAX         2048   /* one outbound message                 */
#define OCPP_TXQ_DEPTH       8      /* outbound queue                       */
#define OCPP_PENDING_MAX     4      /* CALLs awaiting a CALLRESULT          */
#define OCPP_JSON_TOKENS     192

#define OCPP_CALL_TIMEOUT_MS     30000u
#define OCPP_RECONNECT_MIN_MS     5000u
#define OCPP_RECONNECT_MAX_MS   300000u
#define OCPP_PING_INTERVAL_MS    45000u
/** No frame at all for this long means the link is dead regardless of TCP. */
#define OCPP_LINK_DEAD_MS       180000u

/* OCPP message type ids (OCPP-J §4.2). */
#define MSG_CALL        2
#define MSG_CALLRESULT  3
#define MSG_CALLERROR   4

typedef enum {
    OCPP_DISCONNECTED = 0,
    OCPP_CONNECTING,
    OCPP_BOOTING,        /**< WebSocket up, BootNotification outstanding */
    OCPP_READY,
    OCPP_PENDING_RESET
} ocpp_state_t;

/* ====================================================================== */
/* State                                                                  */
/* ====================================================================== */

/** What a pending CALL was, so its response can be routed. */
typedef enum {
    PEND_NONE = 0,
    PEND_BOOT,
    PEND_HEARTBEAT,
    PEND_AUTHORIZE,
    PEND_START_TXN,
    PEND_STOP_TXN,
    PEND_STATUS,
    PEND_METER_VALUES,
    PEND_OTHER
} pend_kind_t;

typedef struct {
    bool        in_use;
    char        id[16];
    pend_kind_t kind;
    uint32_t    sent_ms;
    /** Context the response needs: the tag for Authorize, the local txn id
     *  for StartTransaction. */
    char        id_tag[OCPP_IDTAG_MAXLEN];
    uint32_t    local_txn_id;
} pending_t;

static ws_client_t *s_ws;
static uint8_t      s_rx_buf[OCPP_RX_BUF_SIZE];
static uint8_t      s_tx_buf[OCPP_TX_BUF_SIZE];

static ocpp_state_t s_state;
static uint32_t     s_msg_counter;
static pending_t    s_pending[OCPP_PENDING_MAX];

/* Outbound queue. The EVSE task enqueues; only the OCPP task dequeues. */
static char             s_txq[OCPP_TXQ_DEPTH][OCPP_MSG_MAX];
static uint8_t          s_txq_head, s_txq_tail;
static SemaphoreHandle_t s_txq_lock;
static StaticSemaphore_t s_txq_lock_mem;

static uint32_t s_heartbeat_interval_s = 300u;
static uint32_t s_last_heartbeat_ms;
static uint32_t s_last_meter_values_ms;
static uint32_t s_last_ping_ms;
static uint32_t s_reconnect_delay_ms = OCPP_RECONNECT_MIN_MS;
static uint32_t s_next_connect_ms;
static bool     s_force_reconnect;

/** Set when the CSMS asks for a reset; applied once charging has stopped. */
static bool     s_reset_pending;
static bool     s_reset_hard;

static evse_state_t s_last_reported_state = (evse_state_t)-1;
static uint32_t     s_last_reported_faults;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/** Defined below; declared here so the remote-start handler can use it. */
bool ocpp_parse_charging_profile(const char *js, const json_tok_t *t, int ntok,
                                 int obj, ocpp_profile_t *out);

/* ====================================================================== */
/* Outbound queue                                                         */
/* ====================================================================== */

/**
 * Enqueue a fully formed OCPP frame.
 *
 * Dropping the OLDEST message when the queue is full is deliberate. The queue
 * fills only while offline, and by then the newest StatusNotification and
 * MeterValues describe reality while the oldest do not. Transactions are not
 * affected — those go through the journal, not this queue.
 */
static bool txq_push(const char *msg)
{
    size_t len = strlen(msg);
    if (len == 0u || len >= OCPP_MSG_MAX) return false;

    xSemaphoreTake(s_txq_lock, portMAX_DELAY);
    uint8_t next = (uint8_t)((s_txq_head + 1u) % OCPP_TXQ_DEPTH);
    if (next == s_txq_tail) {
        s_txq_tail = (uint8_t)((s_txq_tail + 1u) % OCPP_TXQ_DEPTH);
    }
    memcpy(s_txq[s_txq_head], msg, len + 1u);
    s_txq_head = next;
    xSemaphoreGive(s_txq_lock);
    return true;
}

static bool txq_pop(char *out, size_t out_len)
{
    bool got = false;
    xSemaphoreTake(s_txq_lock, portMAX_DELAY);
    if (s_txq_tail != s_txq_head) {
        snprintf(out, out_len, "%s", s_txq[s_txq_tail]);
        s_txq_tail = (uint8_t)((s_txq_tail + 1u) % OCPP_TXQ_DEPTH);
        got = true;
    }
    xSemaphoreGive(s_txq_lock);
    return got;
}

/* ====================================================================== */
/* Pending CALL table                                                     */
/* ====================================================================== */

static void next_message_id(char *out, size_t out_len)
{
    snprintf(out, out_len, "%lu", (unsigned long)(++s_msg_counter));
}

static pending_t *pending_alloc(const char *id, pend_kind_t kind)
{
    for (int i = 0; i < OCPP_PENDING_MAX; i++) {
        if (!s_pending[i].in_use) {
            memset(&s_pending[i], 0, sizeof(s_pending[i]));
            s_pending[i].in_use = true;
            s_pending[i].kind = kind;
            s_pending[i].sent_ms = now_ms();
            snprintf(s_pending[i].id, sizeof(s_pending[i].id), "%s", id);
            return &s_pending[i];
        }
    }
    return NULL;
}

static pending_t *pending_find(const char *id)
{
    for (int i = 0; i < OCPP_PENDING_MAX; i++) {
        if (s_pending[i].in_use && strcmp(s_pending[i].id, id) == 0) {
            return &s_pending[i];
        }
    }
    return NULL;
}

/** Expire calls the CSMS never answered, so the table cannot fill up. */
static void pending_expire(void)
{
    uint32_t t = now_ms();
    for (int i = 0; i < OCPP_PENDING_MAX; i++) {
        if (!s_pending[i].in_use) continue;
        if (t - s_pending[i].sent_ms < OCPP_CALL_TIMEOUT_MS) continue;

        /*
         * An unanswered Authorize must not leave the driver waiting forever at
         * the connector. Fail it closed, which sends them back to the app or
         * to a second tag presentation.
         */
        if (s_pending[i].kind == PEND_AUTHORIZE) {
            evse_sm_auth_result(s_pending[i].id_tag, AUTH_REJECTED);
        }
        printf("ocpp: call %s timed out\r\n", s_pending[i].id);
        s_pending[i].in_use = false;
    }
}

/* ====================================================================== */
/* Message construction                                                   */
/* ====================================================================== */

/** Open a CALL frame: [2,"<id>","<action>",{ ... */
static void begin_call(json_writer_t *w, char *buf, size_t cap,
                       const char *action, char *id_out, size_t id_len)
{
    next_message_id(id_out, id_len);
    json_writer_init(w, buf, cap);
    json_arr_open(w);
    json_int(w, MSG_CALL);
    json_str(w, id_out);
    json_str(w, action);
    json_obj_open(w);
}

static void end_call(json_writer_t *w)
{
    json_obj_close(w);
    json_arr_close(w);
}

/** Open a CALLRESULT frame: [3,"<id>",{ ... */
static void begin_result(json_writer_t *w, char *buf, size_t cap, const char *id)
{
    json_writer_init(w, buf, cap);
    json_arr_open(w);
    json_int(w, MSG_CALLRESULT);
    json_str(w, id);
    json_obj_open(w);
}

static void send_result(json_writer_t *w)
{
    json_obj_close(w);
    json_arr_close(w);
    if (json_writer_finish(w) > 0u) {
        txq_push(w->buf);
    }
}

/** [4,"<id>","<code>","<description>",{}] */
static void send_call_error(const char *id, const char *code, const char *desc)
{
    char buf[256];
    json_writer_t w;
    json_writer_init(&w, buf, sizeof(buf));
    json_arr_open(&w);
    json_int(&w, MSG_CALLERROR);
    json_str(&w, id);
    json_str(&w, code);
    json_str(&w, desc ? desc : "");
    json_obj_open(&w);
    json_obj_close(&w);
    json_arr_close(&w);
    if (json_writer_finish(&w) > 0u) txq_push(buf);
}

/** Map the internal state onto an OCPP 1.6 ChargePointStatus. */
static const char *ocpp_status_for(evse_state_t st, uint32_t faults)
{
    if (faults & (EVSE_FAULT_TRIP_MASK | EVSE_FAULT_LATCHING_MASK)) return "Faulted";

    switch (st) {
        case EVSE_STATE_BOOT:
        case EVSE_STATE_IDLE:            return "Available";
        case EVSE_STATE_CONNECTED:
        case EVSE_STATE_PREPARING:       return "Preparing";
        case EVSE_STATE_CHARGING:        return "Charging";
        case EVSE_STATE_SUSPENDED_EV:    return "SuspendedEV";
        case EVSE_STATE_SUSPENDED_EVSE:  return "SuspendedEVSE";
        case EVSE_STATE_FINISHING:       return "Finishing";
        case EVSE_STATE_RESERVED:        return "Reserved";
        case EVSE_STATE_UNAVAILABLE:     return "Unavailable";
        case EVSE_STATE_FAULTED:
        case EVSE_STATE_LOCKOUT:         return "Faulted";
    }
    return "Available";
}

/**
 * Map internal faults onto an OCPP 1.6 ChargePointErrorCode.
 *
 * The enum is small and fixed, so several distinct conditions collapse onto
 * "OtherError". The specific fault name goes in vendorErrorCode, which is what
 * an operator actually needs when diagnosing remotely.
 */
static const char *ocpp_error_code_for(uint32_t faults)
{
    if (faults == 0u)                       return "NoError";
    if (faults & EVSE_FAULT_RCD_TRIP)       return "GroundFailure";
    if (faults & EVSE_FAULT_PE_LOST)        return "GroundFailure";
    if (faults & EVSE_FAULT_PEN)            return "GroundFailure";
    if (faults & EVSE_FAULT_OVER_CURRENT)   return "OverCurrentFailure";
    if (faults & EVSE_FAULT_OVER_VOLTAGE)   return "OverVoltage";
    if (faults & EVSE_FAULT_UNDER_VOLTAGE)  return "UnderVoltage";
    if (faults & EVSE_FAULT_OVER_TEMP)      return "HighTemperature";
    if (faults & EVSE_FAULT_RELAY_WELD)     return "PowerSwitchFailure";
    if (faults & EVSE_FAULT_RELAY_NO_CLOSE) return "PowerSwitchFailure";
    if (faults & EVSE_FAULT_ESTOP)          return "OtherError";
    if (faults & EVSE_FAULT_CP_SHORT)       return "OtherError";
    if (faults & EVSE_FAULT_CP_DIODE)       return "OtherError";
    if (faults & EVSE_FAULT_METER_COMM)     return "ReaderFailure";
    return "OtherError";
}

static void write_timestamp(json_writer_t *w, const char *key, uint32_t unix_s)
{
    char ts[32];
    rtc_format_iso8601(unix_s ? unix_s : rtc_unix_time(), ts, sizeof(ts));
    json_kv_str(w, key, ts);
}

/* ====================================================================== */
/* Outbound messages                                                      */
/* ====================================================================== */

static void send_boot_notification(void)
{
    char buf[OCPP_MSG_MAX], id[16];
    json_writer_t w;

    begin_call(&w, buf, sizeof(buf), "BootNotification", id, sizeof(id));
    json_kv_str(&w, "chargePointVendor", EVSE_VENDOR_NAME);
    json_kv_str(&w, "chargePointModel",  EVSE_MODEL_NAME);
    json_kv_str(&w, "chargePointSerialNumber", cfg_get()->charge_point_id);
    json_kv_str(&w, "firmwareVersion",   EVSE_FW_VERSION);
    end_call(&w);

    if (json_writer_finish(&w) > 0u && pending_alloc(id, PEND_BOOT) != NULL) {
        txq_push(buf);
    }
}

static void send_heartbeat(void)
{
    char buf[128], id[16];
    json_writer_t w;
    begin_call(&w, buf, sizeof(buf), "Heartbeat", id, sizeof(id));
    end_call(&w);
    if (json_writer_finish(&w) > 0u && pending_alloc(id, PEND_HEARTBEAT) != NULL) {
        txq_push(buf);
    }
}

static void send_status_notification(evse_state_t st, uint32_t faults)
{
    char buf[512], id[16];
    json_writer_t w;

    begin_call(&w, buf, sizeof(buf), "StatusNotification", id, sizeof(id));
    json_kv_int(&w, "connectorId", EVSE_CONNECTOR_ID);
    json_kv_str(&w, "errorCode", ocpp_error_code_for(faults));
    json_kv_str(&w, "status", ocpp_status_for(st, faults));
    write_timestamp(&w, "timestamp", 0);
    if (faults != 0u) {
        /* The OCPP error enum is too coarse to diagnose from; the real fault
         * name goes here, which is what shows up in the dashboard. */
        json_kv_str(&w, "vendorErrorCode", evse_fault_name(faults));
        json_kv_str(&w, "vendorId", EVSE_VENDOR_NAME);
    }
    end_call(&w);

    if (json_writer_finish(&w) > 0u && pending_alloc(id, PEND_STATUS) != NULL) {
        txq_push(buf);
    }
}

static void send_authorize(const char *id_tag)
{
    char buf[256], id[16];
    json_writer_t w;

    begin_call(&w, buf, sizeof(buf), "Authorize", id, sizeof(id));
    json_kv_str(&w, "idTag", id_tag);
    end_call(&w);

    pending_t *p = pending_alloc(id, PEND_AUTHORIZE);
    if (json_writer_finish(&w) > 0u && p != NULL) {
        snprintf(p->id_tag, sizeof(p->id_tag), "%s", id_tag);
        txq_push(buf);
    }
}

static void send_start_transaction(const evse_transaction_t *txn)
{
    char buf[512], id[16];
    json_writer_t w;

    begin_call(&w, buf, sizeof(buf), "StartTransaction", id, sizeof(id));
    json_kv_int(&w, "connectorId", EVSE_CONNECTOR_ID);
    json_kv_str(&w, "idTag", txn->id_tag);
    json_kv_int(&w, "meterStart", (int32_t)txn->meter_start_wh);
    write_timestamp(&w, "timestamp", txn->start_time);
    end_call(&w);

    pending_t *p = pending_alloc(id, PEND_START_TXN);
    if (json_writer_finish(&w) > 0u && p != NULL) {
        p->local_txn_id = txn->local_id;
        snprintf(p->id_tag, sizeof(p->id_tag), "%s", txn->id_tag);
        txq_push(buf);
    }
}

static const char *stop_reason_name(stop_reason_t r)
{
    switch (r) {
        case STOP_REASON_LOCAL:           return "Local";
        case STOP_REASON_REMOTE:          return "Remote";
        case STOP_REASON_EV_DISCONNECTED: return "EVDisconnected";
        case STOP_REASON_EMERGENCY_STOP:  return "EmergencyStop";
        case STOP_REASON_POWER_LOSS:      return "PowerLoss";
        case STOP_REASON_REBOOT:          return "Reboot";
        case STOP_REASON_DE_AUTHORIZED:   return "DeAuthorized";
        case STOP_REASON_OTHER:           break;
    }
    return "Other";
}

static void send_stop_transaction(const evse_transaction_t *txn)
{
    char buf[512], id[16];
    json_writer_t w;

    begin_call(&w, buf, sizeof(buf), "StopTransaction", id, sizeof(id));
    json_kv_int(&w, "transactionId", txn->transaction_id);
    json_kv_str(&w, "idTag", txn->id_tag);
    json_kv_int(&w, "meterStop", (int32_t)txn->meter_stop_wh);
    json_kv_str(&w, "reason", stop_reason_name(txn->stop_reason));
    write_timestamp(&w, "timestamp", txn->stop_time);
    end_call(&w);

    if (json_writer_finish(&w) > 0u && pending_alloc(id, PEND_STOP_TXN) != NULL) {
        txq_push(buf);
    }
}

/** One sampledValue object. */
static void sampled(json_writer_t *w, const char *measurand, float value,
                    const char *unit, int decimals)
{
    json_obj_open(w);
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%.*f", decimals, (double)value);
    json_kv_str(w, "value", tmp);
    json_kv_str(w, "measurand", measurand);
    json_kv_str(w, "unit", unit);
    json_obj_close(w);
}

static void send_meter_values(void)
{
    evse_status_t st;
    evse_sm_get_status(&st);
    if (!st.meter.valid) return;

    char buf[OCPP_MSG_MAX], id[16];
    json_writer_t w;

    begin_call(&w, buf, sizeof(buf), "MeterValues", id, sizeof(id));
    json_kv_int(&w, "connectorId", EVSE_CONNECTOR_ID);
    if (st.txn.active && st.txn.transaction_id >= 0) {
        json_kv_int(&w, "transactionId", st.txn.transaction_id);
    }
    json_key(&w, "meterValue");
    json_arr_open(&w);
      json_obj_open(&w);
        write_timestamp(&w, "timestamp", 0);
        json_key(&w, "sampledValue");
        json_arr_open(&w);
          sampled(&w, "Energy.Active.Import.Register",
                  (float)st.meter.energy_wh, "Wh", 0);
          sampled(&w, "Power.Active.Import", st.meter.active_power_w, "W", 1);
          sampled(&w, "Current.Import",      st.meter.current_a,      "A", 2);
          sampled(&w, "Voltage",             st.meter.voltage_v,      "V", 1);
          /* Current.Offered is what makes a "why is my car charging slowly"
           * question answerable from the CSMS alone. */
          sampled(&w, "Current.Offered",     st.offered_a,            "A", 1);
        json_arr_close(&w);
      json_obj_close(&w);
    json_arr_close(&w);
    end_call(&w);

    if (json_writer_finish(&w) > 0u && pending_alloc(id, PEND_METER_VALUES) != NULL) {
        txq_push(buf);
    }
}

/* ====================================================================== */
/* Inbound: CALLRESULT handling                                           */
/* ====================================================================== */

/** Parse an idTagInfo object into a tag entry. */
static void parse_id_tag_info(const char *js, const json_tok_t *t, int ntok,
                              int info, const char *id_tag, ocpp_tag_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->id_tag, sizeof(out->id_tag), "%s", id_tag ? id_tag : "");
    out->status = OCPP_TAG_INVALID;

    if (info < 0) return;

    int st = json_object_get(js, t, ntok, info, "status");
    if (st >= 0) {
        char s[24];
        json_copy_string(js, &t[st], s, sizeof(s));
        out->status = ocpp_auth_status_from_string(s);
    }
    int pid = json_object_get(js, t, ntok, info, "parentIdTag");
    if (pid >= 0) {
        json_copy_string(js, &t[pid], out->parent_id, sizeof(out->parent_id));
    }
    int exp = json_object_get(js, t, ntok, info, "expiryDate");
    if (exp >= 0) {
        char iso[40];
        json_copy_string(js, &t[exp], iso, sizeof(iso));
        out->expiry = rtc_parse_iso8601(iso);
    }
    out->in_use = true;
}

static void handle_call_result(const char *js, const json_tok_t *t, int ntok,
                               pending_t *p, int payload)
{
    switch (p->kind) {

    case PEND_BOOT: {
        int st = json_object_get(js, t, ntok, payload, "status");
        char status[24] = {0};
        if (st >= 0) json_copy_string(js, &t[st], status, sizeof(status));

        int iv = json_object_get(js, t, ntok, payload, "interval");
        int32_t interval = 0;
        if (iv >= 0 && json_get_int(js, &t[iv], &interval) && interval > 0) {
            s_heartbeat_interval_s = (uint32_t)interval;
        }

        /* The CSMS clock is authoritative; ours is not, and transaction
         * timestamps have to agree with it or billing disputes follow. */
        int ct = json_object_get(js, t, ntok, payload, "currentTime");
        if (ct >= 0) {
            char iso[40];
            json_copy_string(js, &t[ct], iso, sizeof(iso));
            uint32_t unix_s = rtc_parse_iso8601(iso);
            if (unix_s > 0u) rtc_set_unix_time(unix_s);
        }

        if (strcmp(status, "Accepted") == 0) {
            s_state = OCPP_READY;
            s_reconnect_delay_ms = OCPP_RECONNECT_MIN_MS;
            printf("ocpp: boot accepted, heartbeat %lus\r\n",
                   (unsigned long)s_heartbeat_interval_s);
            /* Announce our real state now that the CSMS is listening. */
            s_last_reported_state = (evse_state_t)-1;
            txn_log_replay_begin();
        } else {
            /*
             * Pending or Rejected: stay in BOOTING and retry at the interval
             * the CSMS gave us. Sending anything else first would be a protocol
             * violation, and a Pending charger is usually mid-provisioning.
             */
            printf("ocpp: boot %s, retrying in %lus\r\n",
                   status, (unsigned long)s_heartbeat_interval_s);
            s_last_heartbeat_ms = now_ms();
        }
        break;
    }

    case PEND_HEARTBEAT: {
        int ct = json_object_get(js, t, ntok, payload, "currentTime");
        if (ct >= 0) {
            char iso[40];
            json_copy_string(js, &t[ct], iso, sizeof(iso));
            uint32_t unix_s = rtc_parse_iso8601(iso);
            /*
             * Only re-set the clock on a meaningful drift. Writing the RTC on
             * every heartbeat would make timestamps jitter by a second either
             * way across a transaction boundary.
             */
            if (unix_s > 0u) {
                uint32_t local = rtc_unix_time();
                uint32_t drift = (unix_s > local) ? (unix_s - local) : (local - unix_s);
                if (drift > 2u) rtc_set_unix_time(unix_s);
            }
        }
        break;
    }

    case PEND_AUTHORIZE: {
        int info = json_object_get(js, t, ntok, payload, "idTagInfo");
        ocpp_tag_entry_t e;
        parse_id_tag_info(js, t, ntok, info, p->id_tag, &e);
        ocpp_auth_cache_put(e.id_tag, e.parent_id, e.status, e.expiry);
        evse_sm_auth_result(p->id_tag, ocpp_auth_to_result(e.status));
        break;
    }

    case PEND_START_TXN: {
        int tid = json_object_get(js, t, ntok, payload, "transactionId");
        int32_t transaction_id = -1;
        if (tid >= 0) json_get_int(js, &t[tid], &transaction_id);
        evse_sm_set_transaction_id(p->local_txn_id, transaction_id);
        txn_log_set_transaction_id(p->local_txn_id, transaction_id);

        int info = json_object_get(js, t, ntok, payload, "idTagInfo");
        ocpp_tag_entry_t e;
        parse_id_tag_info(js, t, ntok, info, p->id_tag, &e);
        ocpp_auth_cache_put(e.id_tag, e.parent_id, e.status, e.expiry);

        /*
         * The CSMS can refuse a transaction it previously authorised — a
         * concurrent session elsewhere, or a card blocked between Authorize
         * and StartTransaction. Stopping is the only correct response.
         */
        if (e.status != OCPP_TAG_ACCEPTED) {
            printf("ocpp: start rejected (%s), stopping\r\n",
                   ocpp_auth_status_to_string(e.status));
            evse_sm_request_stop(NULL, STOP_REASON_DE_AUTHORIZED);
        }
        break;
    }

    case PEND_STOP_TXN:
        txn_log_confirm_stop(p->local_txn_id);
        ocpp_profile_clear_tx();
        break;

    default:
        break;
    }

    p->in_use = false;
}

/* ====================================================================== */
/* Inbound: CALL handling                                                 */
/* ====================================================================== */

/** Reply with a single "status" field, which most confirmations are. */
static void reply_status(const char *id, const char *status)
{
    char buf[128];
    json_writer_t w;
    begin_result(&w, buf, sizeof(buf), id);
    json_kv_str(&w, "status", status);
    send_result(&w);
}

static void handle_remote_start(const char *js, const json_tok_t *t, int ntok,
                                const char *id, int payload)
{
    char id_tag[OCPP_IDTAG_MAXLEN] = {0};
    int tag = json_object_get(js, t, ntok, payload, "idTag");
    if (tag >= 0) json_copy_string(js, &t[tag], id_tag, sizeof(id_tag));

    if (id_tag[0] == '\0') { reply_status(id, "Rejected"); return; }

    evse_status_t st;
    evse_sm_get_status(&st);
    if (st.txn.active || st.state == EVSE_STATE_FAULTED ||
        st.state == EVSE_STATE_LOCKOUT || st.state == EVSE_STATE_UNAVAILABLE) {
        reply_status(id, "Rejected");
        return;
    }

    /* An optional charging profile may ride along with the remote start. */
    int prof = json_object_get(js, t, ntok, payload, "chargingProfile");
    if (prof >= 0) {
        ocpp_profile_t p;
        if (ocpp_parse_charging_profile(js, t, ntok, prof, &p)) {
            ocpp_profile_set(&p);
        }
    }

    reply_status(id, "Accepted");

    /*
     * AuthorizeRemoteTxRequests is reported as true, so a remote start still
     * goes through the normal authorisation path rather than bypassing it.
     * The CSMS asking is not the same as the CSMS having checked.
     */
    evse_sm_present_tag(id_tag);
}

static void handle_remote_stop(const char *js, const json_tok_t *t, int ntok,
                               const char *id, int payload)
{
    int tid = json_object_get(js, t, ntok, payload, "transactionId");
    int32_t transaction_id = -1;
    if (tid >= 0) json_get_int(js, &t[tid], &transaction_id);

    evse_status_t st;
    evse_sm_get_status(&st);

    if (!st.txn.active || st.txn.transaction_id != transaction_id) {
        reply_status(id, "Rejected");
        return;
    }
    reply_status(id, "Accepted");
    evse_sm_request_stop(NULL, STOP_REASON_REMOTE);
}

static void handle_reset(const char *js, const json_tok_t *t, int ntok,
                         const char *id, int payload)
{
    char type[16] = {0};
    int ty = json_object_get(js, t, ntok, payload, "type");
    if (ty >= 0) json_copy_string(js, &t[ty], type, sizeof(type));

    s_reset_hard = (strcmp(type, "Hard") == 0);
    s_reset_pending = true;
    reply_status(id, "Accepted");

    /*
     * A Soft reset must not yank the contactor out from under a charging car:
     * stop the transaction cleanly first and let the reset happen once the
     * StopTransaction has been sent. A Hard reset is applied regardless, which
     * is what "Hard" means.
     */
    if (evse_sm_transaction_active()) {
        evse_sm_request_stop(NULL, STOP_REASON_REBOOT);
    }
}

static void handle_change_configuration(const char *js, const json_tok_t *t, int ntok,
                                        const char *id, int payload)
{
    char key[64] = {0}, value[128] = {0};
    int k = json_object_get(js, t, ntok, payload, "key");
    int v = json_object_get(js, t, ntok, payload, "value");
    if (k >= 0) json_copy_string(js, &t[k], key, sizeof(key));
    if (v >= 0) json_copy_string(js, &t[v], value, sizeof(value));

    /* HeartbeatInterval is ours to apply live as well as to store. */
    cfg_key_result_t r = cfg_set_ocpp_key(key, value);
    if (r == CFG_KEY_ACCEPTED && strcmp(key, "HeartbeatInterval") == 0) {
        s_heartbeat_interval_s = (uint32_t)atoi(value);
    }

    const char *status = (r == CFG_KEY_ACCEPTED)        ? "Accepted"
                       : (r == CFG_KEY_REBOOT_REQUIRED) ? "RebootRequired"
                       : (r == CFG_KEY_NOT_SUPPORTED)   ? "NotSupported"
                                                        : "Rejected";
    reply_status(id, status);
}

static void handle_get_configuration(const char *js, const json_tok_t *t, int ntok,
                                     const char *id, int payload)
{
    char buf[OCPP_MSG_MAX];
    json_writer_t w;
    begin_result(&w, buf, sizeof(buf), id);

    /*
     * A request with no "key" array means "everything". With a key array, keys
     * we do not know go into unknownKey rather than being silently omitted —
     * the CSMS needs to be able to tell "not supported" from "not answered".
     */
    int keys = json_object_get(js, t, ntok, payload, "key");
    bool all = (keys < 0);

    json_key(&w, "configurationKey");
    json_arr_open(&w);
    for (size_t i = 0; ; i++) {
        const char *name = cfg_ocpp_key_at(i);
        if (name == NULL) break;

        if (!all) {
            bool requested = false;
            for (int n = 0; ; n++) {
                int e = json_array_get(t, ntok, keys, n);
                if (e < 0) break;
                if (json_equals(js, &t[e], name)) { requested = true; break; }
            }
            if (!requested) continue;
        }

        char value[128];
        bool readonly = false;
        if (!cfg_get_ocpp_key(name, value, sizeof(value), &readonly)) continue;

        json_obj_open(&w);
        json_kv_str(&w, "key", name);
        json_kv_bool(&w, "readonly", readonly);
        json_kv_str(&w, "value", value);
        json_obj_close(&w);
    }
    json_arr_close(&w);

    if (!all) {
        bool any_unknown = false;
        for (int n = 0; ; n++) {
            int e = json_array_get(t, ntok, keys, n);
            if (e < 0) break;
            char name[64];
            json_copy_string(js, &t[e], name, sizeof(name));
            char scratch[128];
            if (cfg_get_ocpp_key(name, scratch, sizeof(scratch), NULL)) continue;
            if (!any_unknown) { json_key(&w, "unknownKey"); json_arr_open(&w); any_unknown = true; }
            json_str(&w, name);
        }
        if (any_unknown) json_arr_close(&w);
    }

    send_result(&w);
}

static void handle_change_availability(const char *js, const json_tok_t *t, int ntok,
                                       const char *id, int payload)
{
    char type[16] = {0};
    int ty = json_object_get(js, t, ntok, payload, "type");
    if (ty >= 0) json_copy_string(js, &t[ty], type, sizeof(type));

    bool operative = (strcmp(type, "Operative") == 0);
    evse_sm_set_available(operative);

    /*
     * Taking a connector out of service while a car is charging is "Scheduled",
     * not "Accepted": the change happens when the session ends. Reporting
     * Accepted would tell the operator the connector is already free.
     */
    if (!operative && evse_sm_transaction_active()) {
        reply_status(id, "Scheduled");
    } else {
        reply_status(id, "Accepted");
    }
}

static void handle_trigger_message(const char *js, const json_tok_t *t, int ntok,
                                   const char *id, int payload)
{
    char req[40] = {0};
    int rq = json_object_get(js, t, ntok, payload, "requestedMessage");
    if (rq >= 0) json_copy_string(js, &t[rq], req, sizeof(req));

    evse_status_t st;
    evse_sm_get_status(&st);

    if (strcmp(req, "BootNotification") == 0)          { reply_status(id, "Accepted"); send_boot_notification(); }
    else if (strcmp(req, "Heartbeat") == 0)            { reply_status(id, "Accepted"); send_heartbeat(); }
    else if (strcmp(req, "StatusNotification") == 0)   { reply_status(id, "Accepted"); send_status_notification(st.state, st.faults); }
    else if (strcmp(req, "MeterValues") == 0)          { reply_status(id, "Accepted"); send_meter_values(); }
    else                                                 reply_status(id, "NotImplemented");
}

/* ---- SmartCharging ---- */

static ocpp_purpose_t purpose_from_string(const char *s)
{
    if (strcmp(s, "ChargePointMaxProfile") == 0) return OCPP_PURPOSE_CHARGE_POINT_MAX;
    if (strcmp(s, "TxDefaultProfile") == 0)      return OCPP_PURPOSE_TX_DEFAULT;
    if (strcmp(s, "TxProfile") == 0)             return OCPP_PURPOSE_TX;
    return OCPP_PURPOSE_INVALID;
}

bool ocpp_parse_charging_profile(const char *js, const json_tok_t *t, int ntok,
                                 int obj, ocpp_profile_t *out)
{
    if (obj < 0 || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->transaction_id = -1;

    int v;
    int32_t iv;
    char s[40];

    v = json_object_get(js, t, ntok, obj, "chargingProfileId");
    if (v < 0 || !json_get_int(js, &t[v], &out->profile_id)) return false;

    v = json_object_get(js, t, ntok, obj, "stackLevel");
    if (v >= 0 && json_get_int(js, &t[v], &iv)) out->stack_level = iv;

    v = json_object_get(js, t, ntok, obj, "chargingProfilePurpose");
    if (v < 0) return false;
    json_copy_string(js, &t[v], s, sizeof(s));
    out->purpose = purpose_from_string(s);
    if (out->purpose == OCPP_PURPOSE_INVALID) return false;

    v = json_object_get(js, t, ntok, obj, "chargingProfileKind");
    if (v >= 0) {
        json_copy_string(js, &t[v], s, sizeof(s));
        out->kind = (strcmp(s, "Recurring") == 0) ? OCPP_KIND_RECURRING
                  : (strcmp(s, "Relative")  == 0) ? OCPP_KIND_RELATIVE
                                                  : OCPP_KIND_ABSOLUTE;
    }

    v = json_object_get(js, t, ntok, obj, "recurrencyKind");
    if (v >= 0) {
        json_copy_string(js, &t[v], s, sizeof(s));
        out->recurrency = (strcmp(s, "Weekly") == 0) ? OCPP_RECUR_WEEKLY : OCPP_RECUR_DAILY;
    }

    v = json_object_get(js, t, ntok, obj, "transactionId");
    if (v >= 0 && json_get_int(js, &t[v], &iv)) out->transaction_id = iv;

    v = json_object_get(js, t, ntok, obj, "validFrom");
    if (v >= 0) { json_copy_string(js, &t[v], s, sizeof(s)); out->valid_from = rtc_parse_iso8601(s); }
    v = json_object_get(js, t, ntok, obj, "validTo");
    if (v >= 0) { json_copy_string(js, &t[v], s, sizeof(s)); out->valid_to = rtc_parse_iso8601(s); }

    int sched = json_object_get(js, t, ntok, obj, "chargingSchedule");
    if (sched < 0) return false;

    v = json_object_get(js, t, ntok, sched, "duration");
    if (v >= 0 && json_get_int(js, &t[v], &iv)) out->duration_s = iv;

    v = json_object_get(js, t, ntok, sched, "startSchedule");
    if (v >= 0) { json_copy_string(js, &t[v], s, sizeof(s)); out->start_schedule = rtc_parse_iso8601(s); }

    v = json_object_get(js, t, ntok, sched, "chargingRateUnit");
    if (v >= 0) {
        json_copy_string(js, &t[v], s, sizeof(s));
        out->rate_unit = (s[0] == 'W' || s[0] == 'w') ? OCPP_RATE_WATTS : OCPP_RATE_AMPS;
    }

    v = json_object_get(js, t, ntok, sched, "minChargingRate");
    if (v >= 0) json_get_float(js, &t[v], &out->min_charging_rate);

    int periods = json_object_get(js, t, ntok, sched, "chargingSchedulePeriod");
    if (periods < 0) return false;

    for (int n = 0; n < OCPP_MAX_PERIODS; n++) {
        int e = json_array_get(t, ntok, periods, n);
        if (e < 0) break;

        ocpp_period_t *pd = &out->periods[out->period_count];
        memset(pd, 0, sizeof(*pd));

        v = json_object_get(js, t, ntok, e, "startPeriod");
        if (v < 0 || !json_get_int(js, &t[v], &pd->start_period)) continue;
        v = json_object_get(js, t, ntok, e, "limit");
        if (v < 0 || !json_get_float(js, &t[v], &pd->limit)) continue;
        v = json_object_get(js, t, ntok, e, "numberPhases");
        if (v >= 0 && json_get_int(js, &t[v], &iv)) pd->phases = iv;

        out->period_count++;
    }

    out->valid = (out->period_count > 0u);
    return out->valid;
}

static void handle_set_charging_profile(const char *js, const json_tok_t *t, int ntok,
                                        const char *id, int payload)
{
    int prof = json_object_get(js, t, ntok, payload, "csChargingProfiles");
    ocpp_profile_t p;

    if (!ocpp_parse_charging_profile(js, t, ntok, prof, &p)) {
        reply_status(id, "Rejected");
        return;
    }
    reply_status(id, ocpp_profile_set(&p) ? "Accepted" : "Rejected");
}

static void handle_clear_charging_profile(const char *js, const json_tok_t *t, int ntok,
                                          const char *id, int payload)
{
    int32_t profile_id = -1, stack_level = -1;
    ocpp_purpose_t purpose = OCPP_PURPOSE_INVALID;

    int v = json_object_get(js, t, ntok, payload, "id");
    if (v >= 0) json_get_int(js, &t[v], &profile_id);
    v = json_object_get(js, t, ntok, payload, "stackLevel");
    if (v >= 0) json_get_int(js, &t[v], &stack_level);
    v = json_object_get(js, t, ntok, payload, "chargingProfilePurpose");
    if (v >= 0) {
        char s[40];
        json_copy_string(js, &t[v], s, sizeof(s));
        purpose = purpose_from_string(s);
    }

    int n = ocpp_profile_clear(profile_id, purpose, stack_level);
    reply_status(id, (n > 0) ? "Accepted" : "Unknown");
}

/* ---- Local auth list ---- */

static void handle_send_local_list(const char *js, const json_tok_t *t, int ntok,
                                   const char *id, int payload)
{
    int32_t version = 0;
    int v = json_object_get(js, t, ntok, payload, "listVersion");
    if (v < 0 || !json_get_int(js, &t[v], &version)) {
        reply_status(id, "Failed");
        return;
    }

    char type[16] = {0};
    v = json_object_get(js, t, ntok, payload, "updateType");
    if (v >= 0) json_copy_string(js, &t[v], type, sizeof(type));
    bool full = (strcmp(type, "Full") == 0);

    ocpp_auth_list_begin(version, full);

    int arr = json_object_get(js, t, ntok, payload, "localAuthorizationList");
    bool ok = true;
    for (int n = 0; arr >= 0; n++) {
        int e = json_array_get(t, ntok, arr, n);
        if (e < 0) break;

        ocpp_tag_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        int tag = json_object_get(js, t, ntok, e, "idTag");
        if (tag < 0) continue;
        json_copy_string(js, &t[tag], entry.id_tag, sizeof(entry.id_tag));

        /*
         * An entry with no idTagInfo is a deletion. That is how a Differential
         * update revokes a card, so treating a missing idTagInfo as a
         * malformed entry would leave revoked cards working.
         */
        int info = json_object_get(js, t, ntok, e, "idTagInfo");
        if (info < 0) {
            ok = ocpp_auth_list_put(&entry, true) && ok;
            continue;
        }

        parse_id_tag_info(js, t, ntok, info, entry.id_tag, &entry);
        ok = ocpp_auth_list_put(&entry, false) && ok;
    }

    if (ok) {
        ocpp_auth_list_commit();
        reply_status(id, "Accepted");
    } else {
        /* Out of room. Leave the previous list intact rather than committing a
         * partial one, and say so. */
        ocpp_auth_list_abort();
        reply_status(id, "Failed");
    }
}

static void handle_get_local_list_version(const char *id)
{
    char buf[96];
    json_writer_t w;
    begin_result(&w, buf, sizeof(buf), id);
    json_kv_int(&w, "listVersion", ocpp_auth_list_version());
    send_result(&w);
}

/* ---- Reservation ---- */

static void handle_reserve_now(const char *js, const json_tok_t *t, int ntok,
                               const char *id, int payload)
{
    int32_t reservation_id = -1;
    char id_tag[OCPP_IDTAG_MAXLEN] = {0}, expiry_iso[40] = {0};

    int v = json_object_get(js, t, ntok, payload, "reservationId");
    if (v >= 0) json_get_int(js, &t[v], &reservation_id);
    v = json_object_get(js, t, ntok, payload, "idTag");
    if (v >= 0) json_copy_string(js, &t[v], id_tag, sizeof(id_tag));
    v = json_object_get(js, t, ntok, payload, "expiryDate");
    if (v >= 0) json_copy_string(js, &t[v], expiry_iso, sizeof(expiry_iso));

    uint32_t expiry = rtc_parse_iso8601(expiry_iso);
    if (expiry == 0u) { reply_status(id, "Rejected"); return; }

    evse_status_t st;
    evse_sm_get_status(&st);
    if (st.state == EVSE_STATE_FAULTED || st.state == EVSE_STATE_LOCKOUT) {
        reply_status(id, "Faulted");
    } else if (st.state == EVSE_STATE_UNAVAILABLE) {
        reply_status(id, "Unavailable");
    } else if (st.txn.active || st.state != EVSE_STATE_IDLE) {
        reply_status(id, "Occupied");
    } else {
        reply_status(id, evse_sm_reserve(reservation_id, id_tag, expiry)
                         ? "Accepted" : "Rejected");
    }
}

static void handle_cancel_reservation(const char *js, const json_tok_t *t, int ntok,
                                      const char *id, int payload)
{
    int32_t reservation_id = -1;
    int v = json_object_get(js, t, ntok, payload, "reservationId");
    if (v >= 0) json_get_int(js, &t[v], &reservation_id);
    evse_sm_cancel_reservation(reservation_id);
    reply_status(id, "Accepted");
}

/* ---- Dispatch ---- */

typedef void (*call_handler_t)(const char *js, const json_tok_t *t, int ntok,
                               const char *id, int payload);

typedef struct { const char *action; call_handler_t fn; } call_entry_t;

static void h_clear_cache(const char *js, const json_tok_t *t, int ntok,
                          const char *id, int payload)
{
    (void)js; (void)t; (void)ntok; (void)payload;
    ocpp_auth_cache_clear();
    reply_status(id, "Accepted");
}

static void h_unlock_connector(const char *js, const json_tok_t *t, int ntok,
                               const char *id, int payload)
{
    (void)js; (void)t; (void)ntok; (void)payload;
    /*
     * This is a J1772 unit with no motorised latch — the release is the button
     * on the plug handle. "NotSupported" is the honest answer; replying
     * "Unlocked" would tell an operator a driver had been released when
     * nothing happened.
     */
    reply_status(id, "NotSupported");
}

static void h_get_local_list_version(const char *js, const json_tok_t *t, int ntok,
                                     const char *id, int payload)
{
    (void)js; (void)t; (void)ntok; (void)payload;
    handle_get_local_list_version(id);
}

static void h_data_transfer(const char *js, const json_tok_t *t, int ntok,
                            const char *id, int payload)
{
    (void)js; (void)t; (void)ntok; (void)payload;
    reply_status(id, "UnknownVendorId");
}

static const call_entry_t CALL_TABLE[] = {
    { "RemoteStartTransaction", handle_remote_start          },
    { "RemoteStopTransaction",  handle_remote_stop           },
    { "Reset",                  handle_reset                 },
    { "ChangeConfiguration",    handle_change_configuration  },
    { "GetConfiguration",       handle_get_configuration     },
    { "ChangeAvailability",     handle_change_availability   },
    { "TriggerMessage",         handle_trigger_message       },
    { "SetChargingProfile",     handle_set_charging_profile  },
    { "ClearChargingProfile",   handle_clear_charging_profile},
    { "SendLocalList",          handle_send_local_list       },
    { "GetLocalListVersion",    h_get_local_list_version     },
    { "ReserveNow",             handle_reserve_now           },
    { "CancelReservation",      handle_cancel_reservation    },
    { "ClearCache",             h_clear_cache                },
    { "UnlockConnector",        h_unlock_connector           },
    { "DataTransfer",           h_data_transfer              },
};

#define CALL_TABLE_LEN (sizeof(CALL_TABLE)/sizeof(CALL_TABLE[0]))

/* ====================================================================== */
/* Frame dispatch                                                         */
/* ====================================================================== */

static void handle_message(const char *msg, size_t len)
{
    static json_tok_t toks[OCPP_JSON_TOKENS];

    int ntok = json_parse(msg, len, toks, OCPP_JSON_TOKENS);
    if (ntok <= 0 || toks[0].type != JSON_ARRAY) {
        printf("ocpp: unparseable frame (%d)\r\n", ntok);
        return;
    }

    int e0 = json_array_get(toks, ntok, 0, 0);
    int e1 = json_array_get(toks, ntok, 0, 1);
    int32_t msg_type = 0;
    if (e0 < 0 || e1 < 0 || !json_get_int(msg, &toks[e0], &msg_type)) return;

    char id[16];
    json_copy_string(msg, &toks[e1], id, sizeof(id));

    switch (msg_type) {

    case MSG_CALL: {
        int e2 = json_array_get(toks, ntok, 0, 2);
        int e3 = json_array_get(toks, ntok, 0, 3);
        if (e2 < 0) return;

        char action[40];
        json_copy_string(msg, &toks[e2], action, sizeof(action));

        for (size_t i = 0; i < CALL_TABLE_LEN; i++) {
            if (strcmp(CALL_TABLE[i].action, action) == 0) {
                CALL_TABLE[i].fn(msg, toks, ntok, id, e3);
                return;
            }
        }
        /*
         * OCPP-J requires a CALLERROR for an action we do not implement. Not
         * answering leaves the CSMS waiting for its own timeout, which on some
         * implementations tears down the whole session.
         */
        printf("ocpp: unsupported action %s\r\n", action);
        send_call_error(id, "NotImplemented", action);
        break;
    }

    case MSG_CALLRESULT: {
        pending_t *p = pending_find(id);
        if (p == NULL) {
            printf("ocpp: result for unknown call %s\r\n", id);
            return;
        }
        int e2 = json_array_get(toks, ntok, 0, 2);
        handle_call_result(msg, toks, ntok, p, e2);
        break;
    }

    case MSG_CALLERROR: {
        pending_t *p = pending_find(id);
        int e2 = json_array_get(toks, ntok, 0, 2);
        char code[40] = {0};
        if (e2 >= 0) json_copy_string(msg, &toks[e2], code, sizeof(code));
        printf("ocpp: CALLERROR %s for %s\r\n", code, id);

        if (p != NULL) {
            /* Fail an outstanding authorisation closed rather than leaving the
             * driver waiting at the connector. */
            if (p->kind == PEND_AUTHORIZE) {
                evse_sm_auth_result(p->id_tag, AUTH_REJECTED);
            }
            p->in_use = false;
        }
        break;
    }

    default:
        break;
    }
}

/* ====================================================================== */
/* State-machine callbacks                                                */
/* ====================================================================== */

static void cb_state_change(evse_state_t from, evse_state_t to, uint32_t faults)
{
    (void)from;
    send_status_notification(to, faults);
}

static void cb_txn_start(const evse_transaction_t *txn)
{
    /* Journal first, transmit second. If power is lost between the two, the
     * session is still recoverable; the other order loses it. */
    txn_log_record_start(txn);
    send_start_transaction(txn);
}

static void cb_txn_stop(const evse_transaction_t *txn)
{
    txn_log_record_stop(txn);
    send_stop_transaction(txn);
    ocpp_profile_clear_tx();
}

static void cb_request_authorize(const char *id_tag)
{
    const evse_config_t *cfg = cfg_get();
    uint32_t now = rtc_unix_time();

    /*
     * Try locally first. The local list is authoritative and answers instantly;
     * only fall through to the CSMS when there is no local decision.
     */
    ocpp_tag_entry_t e;
    bool offline = !ocpp_is_connected();
    bool allow_cache = offline ? cfg->allow_offline_charging : true;

    if (ocpp_auth_lookup(id_tag, now, allow_cache, &e)) {
        evse_sm_auth_result(id_tag, ocpp_auth_to_result(e.status));
        return;
    }

    if (offline) {
        /*
         * Unknown tag with no link. AllowOfflineTxForUnknownId decides, and
         * defaulting it to "accept" would make an offline charger a free
         * charger — so it is a deliberate configuration choice, not a default.
         */
        evse_sm_auth_result(id_tag,
            cfg->allow_offline_charging ? AUTH_ACCEPTED : AUTH_REJECTED);
        return;
    }
    send_authorize(id_tag);
}

static const evse_sm_callbacks_t SM_CALLBACKS = {
    .on_state_change   = cb_state_change,
    .on_txn_start      = cb_txn_start,
    .on_txn_stop       = cb_txn_stop,
    .request_authorize = cb_request_authorize,
};

const evse_sm_callbacks_t *ocpp_get_sm_callbacks(void) { return &SM_CALLBACKS; }

/* ====================================================================== */
/* Task                                                                   */
/* ====================================================================== */

bool ocpp_is_connected(void) { return s_state == OCPP_READY; }

void ocpp_client_reconnect(void) { s_force_reconnect = true; }

const char *ocpp_state_name(void)
{
    switch (s_state) {
        case OCPP_DISCONNECTED:  return "Disconnected";
        case OCPP_CONNECTING:    return "Connecting";
        case OCPP_BOOTING:       return "Booting";
        case OCPP_READY:         return "Ready";
        case OCPP_PENDING_RESET: return "Resetting";
    }
    return "?";
}

void ocpp_client_start(void)
{
    s_txq_lock = xSemaphoreCreateMutexStatic(&s_txq_lock_mem);
    ocpp_profile_init();
    ocpp_auth_init();
    txn_log_init();
    s_state = OCPP_DISCONNECTED;
    s_next_connect_ms = 0;
}

/** Apply the smart-charging limit to the state machine. */
static void push_profile_limit(void)
{
    evse_status_t st;
    evse_sm_get_status(&st);

    uint32_t now = rtc_unix_time();
    uint32_t txn_start = st.txn.active ? st.txn.start_time : 0u;

    /*
     * Fallback is "no profile limit", expressed as the hardware maximum, so a
     * charger with no profiles installed is not silently throttled.
     */
    float limit = ocpp_profile_limit_a(now, txn_start,
                                       EVSE_NOMINAL_VOLTAGE_V,
                                       (uint8_t)EVSE_PHASE_COUNT,
                                       EVSE_MAX_CURRENT_A);
    evse_sm_set_profile_limit(limit);
}

static void do_connect(void)
{
    const evse_config_t *cfg = cfg_get();

    if (!net_link_is_up()) return;
    if ((int32_t)(now_ms() - s_next_connect_ms) < 0) return;

    s_state = OCPP_CONNECTING;
    printf("ocpp: connecting to %s\r\n", cfg->csms_url);

    s_ws = ws_create(s_rx_buf, sizeof(s_rx_buf), s_tx_buf, sizeof(s_tx_buf));

    const char *user = (cfg->security_profile >= 1u) ? cfg->charge_point_id : NULL;
    const char *pass = (cfg->security_profile >= 1u) ? cfg->auth_key : NULL;

    if (!ws_connect(s_ws, cfg->csms_url, "ocpp1.6", user, pass)) {
        /*
         * Exponential backoff, capped. A site whose CSMS is down should not
         * have every charger retrying every five seconds forever — that is how
         * a backend outage turns into a thundering herd when it recovers.
         */
        s_state = OCPP_DISCONNECTED;
        s_next_connect_ms = now_ms() + s_reconnect_delay_ms;
        s_reconnect_delay_ms *= 2u;
        if (s_reconnect_delay_ms > OCPP_RECONNECT_MAX_MS) {
            s_reconnect_delay_ms = OCPP_RECONNECT_MAX_MS;
        }
        printf("ocpp: connect failed, retry in %lus\r\n",
               (unsigned long)(s_reconnect_delay_ms / 1000u));
        return;
    }

    s_state = OCPP_BOOTING;
    s_last_heartbeat_ms = now_ms();
    s_last_ping_ms = now_ms();
    send_boot_notification();
}

static void drop_connection(const char *why)
{
    printf("ocpp: disconnected (%s)\r\n", why);
    if (s_ws) ws_close(s_ws, 1000);
    s_state = OCPP_DISCONNECTED;
    s_next_connect_ms = now_ms() + s_reconnect_delay_ms;

    /* Outstanding calls will never be answered now. */
    for (int i = 0; i < OCPP_PENDING_MAX; i++) {
        if (s_pending[i].in_use && s_pending[i].kind == PEND_AUTHORIZE) {
            evse_sm_auth_result(s_pending[i].id_tag, AUTH_REJECTED);
        }
        s_pending[i].in_use = false;
    }
}

void task_ocpp(void *arg)
{
    (void)arg;

    net_link_init(cfg_get());

    for (;;) {
        net_link_poll();
        pending_expire();
        push_profile_limit();

        if (s_force_reconnect && s_state != OCPP_DISCONNECTED) {
            s_force_reconnect = false;
            drop_connection("requested");
        }

        if (s_state == OCPP_DISCONNECTED) {
            do_connect();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* ---- Periodic traffic ---- */
        uint32_t t = now_ms();

        if (s_state == OCPP_READY || s_state == OCPP_BOOTING) {
            if (t - s_last_heartbeat_ms >= s_heartbeat_interval_s * 1000u) {
                s_last_heartbeat_ms = t;
                if (s_state == OCPP_READY) send_heartbeat();
                else                        send_boot_notification();
            }
        }

        if (s_state == OCPP_READY) {
            uint32_t interval = cfg_get()->meter_value_interval_s;
            if (interval > 0u && t - s_last_meter_values_ms >= interval * 1000u) {
                s_last_meter_values_ms = t;
                send_meter_values();
            }

            /* Report state changes as they happen rather than on a timer. */
            evse_status_t st;
            evse_sm_get_status(&st);
            if (st.state != s_last_reported_state || st.faults != s_last_reported_faults) {
                s_last_reported_state = st.state;
                s_last_reported_faults = st.faults;
                send_status_notification(st.state, st.faults);
            }

            txn_log_replay_step();
        }

        if (t - s_last_ping_ms >= OCPP_PING_INTERVAL_MS) {
            s_last_ping_ms = t;
            ws_ping(s_ws);
        }

        /*
         * A TCP connection to a CSMS behind a NAT or a mobile link can stay
         * "open" long after the far end has gone. Nothing arriving at all —
         * not even a pong — is the only reliable signal.
         */
        if (ws_idle_ms(s_ws) > OCPP_LINK_DEAD_MS) {
            drop_connection("no traffic");
            continue;
        }

        /* ---- Drain the outbound queue ---- */
        char outbuf[OCPP_MSG_MAX];
        while (txq_pop(outbuf, sizeof(outbuf))) {
            if (!ws_send_text(s_ws, outbuf, strlen(outbuf))) {
                drop_connection("send failed");
                break;
            }
        }
        if (s_state == OCPP_DISCONNECTED) continue;

        /* ---- Receive ---- */
        const char *msg = NULL;
        size_t msg_len = 0;
        ws_event_t ev = ws_poll(s_ws, 100, &msg, &msg_len);

        if (ev == WS_EVT_TEXT && msg != NULL) {
            handle_message(msg, msg_len);
        } else if (ev == WS_EVT_CLOSED || ev == WS_EVT_ERROR) {
            drop_connection("peer closed");
            continue;
        }

        /* ---- Deferred reset ---- */
        if (s_reset_pending && (s_reset_hard || !evse_sm_transaction_active())) {
            /* Give the StopTransaction a moment to leave before rebooting. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            printf("ocpp: %s reset\r\n", s_reset_hard ? "hard" : "soft");
            ws_close(s_ws, 1001);
            NVIC_SystemReset();
        }
    }
}

/* ====================================================================== */
/* Journal replay entry points                                            */
/*                                                                        */
/* txn_log.c calls these to re-emit stored transactions. They are the same */
/* builders the live path uses, so a replayed message is byte-identical to */
/* the one that would have been sent at the time -- including its original */
/* timestamp, which is the whole point of the journal.                     */
/* ====================================================================== */

void ocpp_replay_start_transaction(const evse_transaction_t *txn)
{
    printf("ocpp: replaying start of local txn %lu\r\n", (unsigned long)txn->local_id);
    send_start_transaction(txn);
}

void ocpp_replay_stop_transaction(const evse_transaction_t *txn)
{
    printf("ocpp: replaying stop of local txn %lu\r\n", (unsigned long)txn->local_id);
    send_stop_transaction(txn);
}
