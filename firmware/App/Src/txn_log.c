/**
 * @file  txn_log.c
 * @brief Transaction journal. See txn_log.h.
 */
#include "txn_log.h"
#include "evse_board.h"
#include "rtc_time.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifndef EVSE_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#endif

#define TXN_MAGIC        0x544Au    /* "TJ" */

typedef enum {
    REC_FREE  = 0xFF,   /* erased flash reads as 0xFF */
    REC_START = 1,
    REC_STOP  = 2,
    REC_ACK   = 3
} rec_type_t;

/**
 * 64 bytes so records stay quad-word aligned for the H5 flash programmer and
 * a 16 KB region holds 256 of them — comfortably more than a charger will
 * accumulate across any realistic outage.
 */
typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint8_t  reason;
    uint32_t local_id;
    int32_t  transaction_id;
    uint32_t timestamp;         /**< Unix seconds, as it happened */
    uint32_t meter_wh;
    char     id_tag[OCPP_IDTAG_MAXLEN];   /* 21 bytes, ends at offset 41 */
    uint8_t  pad[19];                     /* aligns crc to offset 60     */
    uint32_t crc;
} txn_record_t;

_Static_assert(sizeof(txn_record_t) == 64, "txn_record_t must stay 64 bytes");

#define TXN_SLOT_COUNT   (NVM_TXNLOG_SIZE / sizeof(txn_record_t))

static uint32_t s_write_index;      /* next free slot */
static size_t   s_replay_index;
static bool     s_replaying;

/* Set by ocpp_client.c so the journal can re-emit without a circular include. */
void ocpp_replay_start_transaction(const evse_transaction_t *txn);
void ocpp_replay_stop_transaction(const evse_transaction_t *txn);

static uint32_t crc32_calc(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static const txn_record_t *slot(uint32_t i)
{
    return (const txn_record_t *)(NVM_TXNLOG_ADDR + i * sizeof(txn_record_t));
}

static bool slot_valid(const txn_record_t *r)
{
    if (r->magic != TXN_MAGIC) return false;
    if (r->type == REC_FREE)   return false;
    return r->crc == crc32_calc(r, offsetof(txn_record_t, crc));
}

#ifndef EVSE_HOST_TEST

static bool flash_erase_region(void)
{
    FLASH_EraseInitTypeDef er = {0};
    uint32_t err = 0;
    er.TypeErase = FLASH_TYPEERASE_SECTORS;
    er.Banks     = FLASH_BANK_2;
    er.Sector    = (NVM_TXNLOG_ADDR - (FLASH_BASE + FLASH_BANK_SIZE)) / NVM_SECTOR_SIZE;
    er.NbSectors = NVM_TXNLOG_SIZE / NVM_SECTOR_SIZE;
    if (HAL_FLASH_Unlock() != HAL_OK) return false;
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &err);
    HAL_FLASH_Lock();
    return st == HAL_OK;
}

static bool flash_write_record(uint32_t index, const txn_record_t *r)
{
    uint32_t addr = NVM_TXNLOG_ADDR + index * sizeof(txn_record_t);
    if (HAL_FLASH_Unlock() != HAL_OK) return false;

    bool ok = true;
    for (size_t off = 0; off < sizeof(*r); off += 16u) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, addr + off,
                              (uint32_t)(uintptr_t)((const uint8_t *)r + off)) != HAL_OK) {
            ok = false;
            break;
        }
    }
    HAL_FLASH_Lock();
    return ok;
}

#else  /* host tests: a RAM-backed fake */
static uint8_t s_fake_flash[NVM_TXNLOG_SIZE];
static bool flash_erase_region(void) { memset(s_fake_flash, 0xFF, sizeof(s_fake_flash)); return true; }
static bool flash_write_record(uint32_t i, const txn_record_t *r)
{ memcpy(&s_fake_flash[i * sizeof(*r)], r, sizeof(*r)); return true; }
#endif

/** True if @p local_id already has an ACK record. */
static bool is_acked(uint32_t local_id)
{
    for (uint32_t i = 0; i < s_write_index; i++) {
        const txn_record_t *r = slot(i);
        if (slot_valid(r) && r->type == REC_ACK && r->local_id == local_id) {
            return true;
        }
    }
    return false;
}

/**
 * Copy every unacknowledged record to the front of a freshly erased region.
 *
 * Called when the log fills. Anything already acknowledged is dropped, which
 * is normally almost all of it — a charger that has been online has no
 * outstanding records at all.
 */
static void compact(void)
{
    static txn_record_t keep[32];
    size_t n = 0;

    for (uint32_t i = 0; i < TXN_SLOT_COUNT && n < 32u; i++) {
        const txn_record_t *r = slot(i);
        if (!slot_valid(r) || r->type == REC_ACK) continue;
        if (is_acked(r->local_id)) continue;
        keep[n++] = *r;
    }

    if (!flash_erase_region()) {
        printf("txnlog: erase failed\r\n");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        flash_write_record((uint32_t)i, &keep[i]);
    }
    s_write_index = (uint32_t)n;
    printf("txnlog: compacted, %u records kept\r\n", (unsigned)n);
}

static void append(rec_type_t type, const evse_transaction_t *txn, uint32_t local_id)
{
    if (s_write_index >= TXN_SLOT_COUNT) {
        compact();
        if (s_write_index >= TXN_SLOT_COUNT) {
            printf("txnlog: full, dropping record\r\n");
            return;
        }
    }

    txn_record_t r;
    memset(&r, 0xFF, sizeof(r));      /* match erased flash in the padding */
    r.magic = TXN_MAGIC;
    r.type  = (uint8_t)type;

    if (txn != NULL) {
        r.local_id       = txn->local_id;
        r.transaction_id = txn->transaction_id;
        r.reason         = (uint8_t)txn->stop_reason;
        r.timestamp      = (type == REC_STOP) ? txn->stop_time : txn->start_time;
        r.meter_wh       = (uint32_t)((type == REC_STOP) ? txn->meter_stop_wh
                                                         : txn->meter_start_wh);
        memcpy(r.id_tag, txn->id_tag, sizeof(r.id_tag));
    } else {
        r.local_id       = local_id;
        r.transaction_id = -1;
        r.reason         = 0;
        r.timestamp      = rtc_unix_time();
        r.meter_wh       = 0;
        memset(r.id_tag, 0, sizeof(r.id_tag));
    }
    r.crc = crc32_calc(&r, offsetof(txn_record_t, crc));

    if (flash_write_record(s_write_index, &r)) {
        s_write_index++;
    }
}

void txn_log_init(void)
{
    s_write_index = 0;
    s_replaying = false;
    s_replay_index = 0;

    /* The first free slot is the write cursor. */
    for (uint32_t i = 0; i < TXN_SLOT_COUNT; i++) {
        const txn_record_t *r = slot(i);
        if (r->magic != TXN_MAGIC) { s_write_index = i; break; }
        s_write_index = i + 1u;
    }

    size_t pending = txn_log_pending_count();
    if (pending > 0u) {
        printf("txnlog: %u unacknowledged record(s) to replay\r\n", (unsigned)pending);
    }
}

void txn_log_record_start(const evse_transaction_t *txn) { append(REC_START, txn, 0); }
void txn_log_record_stop(const evse_transaction_t *txn)  { append(REC_STOP,  txn, 0); }

void txn_log_set_transaction_id(uint32_t local_id, int32_t transaction_id)
{
    /*
     * Flash cannot be edited in place, so the id is not written back into the
     * START record. Replay works without it: a replayed StartTransaction gets
     * a fresh id from the CSMS, and the matching StopTransaction is emitted
     * with whatever id that reply carried.
     */
    (void)local_id;
    (void)transaction_id;
}

void txn_log_confirm_stop(uint32_t local_id)
{
    if (is_acked(local_id)) return;
    append(REC_ACK, NULL, local_id);
}

size_t txn_log_pending_count(void)
{
    size_t n = 0;
    for (uint32_t i = 0; i < s_write_index; i++) {
        const txn_record_t *r = slot(i);
        if (!slot_valid(r) || r->type == REC_ACK) continue;
        if (r->type != REC_STOP) continue;      /* count sessions, not events */
        if (!is_acked(r->local_id)) n++;
    }
    return n;
}

void txn_log_replay_begin(void)
{
    s_replaying = (txn_log_pending_count() > 0u);
    s_replay_index = 0;
}

bool txn_log_replay_step(void)
{
    if (!s_replaying) return false;

    while (s_replay_index < s_write_index) {
        const txn_record_t *r = slot((uint32_t)s_replay_index++);
        if (!slot_valid(r) || r->type == REC_ACK) continue;
        if (is_acked(r->local_id)) continue;

        evse_transaction_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.local_id       = r->local_id;
        txn.transaction_id = r->transaction_id;
        memcpy(txn.id_tag, r->id_tag, sizeof(txn.id_tag));
        txn.id_tag[sizeof(txn.id_tag) - 1] = '\0';

        if (r->type == REC_START) {
            txn.meter_start_wh = r->meter_wh;
            txn.start_time     = r->timestamp;
            ocpp_replay_start_transaction(&txn);
        } else {
            txn.meter_stop_wh = r->meter_wh;
            txn.stop_time     = r->timestamp;
            txn.stop_reason   = (stop_reason_t)r->reason;
            ocpp_replay_stop_transaction(&txn);
        }
        return true;      /* one record per call, to share the outbound queue */
    }

    s_replaying = false;
    return false;
}
