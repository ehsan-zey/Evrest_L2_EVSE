/**
 * @file  evse_config.c
 * @brief Persistent configuration and the installer DIP switches.
 */
#include "evse_config.h"
#include "evse_board.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static evse_config_t s_cfg;

/* ====================================================================== */
/* DIP switches                                                           */
/* ====================================================================== */

/** Index is the 3-bit DIP code; see evse_board.h for the polarity note. */
static const float DIP_CURRENT_TABLE[8] = {
    [0] = 16.0f,   /* 000 */
    [1] = 24.0f,   /* 001 */
    [2] = 32.0f,   /* 010 */
    [3] = 40.0f,   /* 011 */
    [4] = 48.0f,   /* 100 */
    [5] = DIP_RESERVED_CURRENT_A,
    [6] = DIP_RESERVED_CURRENT_A,
    [7] = DIP_RESERVED_CURRENT_A,
};

uint8_t cfg_dip_code(void)
{
    uint8_t code = 0;
    if (HAL_GPIO_ReadPin(DIP_GPIO_PORT, DIP1_GPIO_PIN) == GPIO_PIN_SET) code |= 0x1u;
    if (HAL_GPIO_ReadPin(DIP_GPIO_PORT, DIP2_GPIO_PIN) == GPIO_PIN_SET) code |= 0x2u;
    if (HAL_GPIO_ReadPin(DIP_GPIO_PORT, DIP3_GPIO_PIN) == GPIO_PIN_SET) code |= 0x4u;
#if DIP_CODE_IS_INVERTED
    code = (uint8_t)(~code & 0x7u);
#endif
    return code;
}

float cfg_dip_current_a(void)
{
    float a = DIP_CURRENT_TABLE[cfg_dip_code() & 0x7u];
    /* The DIP can never authorise more than the hardware is rated for. */
    return evse_minf(a, EVSE_MAX_CURRENT_A);
}

/* ====================================================================== */
/* CRC                                                                    */
/* ====================================================================== */

/** CRC-32/ISO-HDLC, computed in software so it does not depend on the CRC
 *  peripheral being clocked or on its configuration surviving a reset. */
static uint32_t crc32_calc(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/** CRC covers everything up to but excluding the crc32 field itself. */
static uint32_t cfg_crc(const evse_config_t *c)
{
    return crc32_calc(c, offsetof(evse_config_t, crc32));
}

static bool cfg_valid(const evse_config_t *c)
{
    return c->magic == CFG_MAGIC
        && c->version == CFG_VERSION
        && c->size == sizeof(evse_config_t)
        && c->crc32 == cfg_crc(c);
}

/* ====================================================================== */
/* Defaults                                                               */
/* ====================================================================== */

void cfg_load_defaults(evse_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic   = CFG_MAGIC;
    cfg->version = CFG_VERSION;
    cfg->size    = (uint16_t)sizeof(evse_config_t);
    cfg->sequence = 1u;

    cfg->max_current_a         = DIP_RESERVED_CURRENT_A;
    cfg->earthing              = EVSE_EARTH_TN_S;
    cfg->connector_type        = CONNECTOR_TYPE1_J1772;
    cfg->ventilation_available = false;
    cfg->phase_count           = EVSE_PHASE_COUNT;

    snprintf(cfg->csms_url, sizeof(cfg->csms_url), "ws://192.168.1.10:9220/ocpp");
    snprintf(cfg->charge_point_id, sizeof(cfg->charge_point_id), "EVREST-0001");
    cfg->auth_key[0]            = '\0';
    cfg->heartbeat_interval_s   = 300u;
    cfg->meter_value_interval_s = 60u;
    cfg->security_profile       = 0u;
    cfg->allow_offline_charging = true;
    cfg->free_vend              = false;
    cfg->stop_txn_on_ev_disconnect = true;

    cfg->iface = NET_IFACE_AUTO;
    cfg->dhcp  = true;

    cfg->cp_cal_scale_num = CP_ADC_SCALE_NUM;
    cfg->cp_cal_scale_den = CP_ADC_SCALE_DEN;
    cfg->cp_cal_offset_mv = CP_ADC_OFFSET_MV;

    cfg->energy_total_wh = 0u;
    cfg->lockout_faults  = 0u;
    cfg->boot_count      = 0u;
    cfg->crc32 = cfg_crc(cfg);
}

/* ====================================================================== */
/* Flash access                                                           */
/* ====================================================================== */

/** Which physical copy we most recently loaded from, so we write the other. */
static uint32_t s_active_addr;

/* uintptr_t rather than a direct cast so this is also clean when built for the
 * host compile-check, where pointers are wider than the flash address. */
static const evse_config_t *cfg_at(uint32_t addr)
{
    return (const evse_config_t *)(uintptr_t)addr;
}

static bool flash_erase_sector(uint32_t addr)
{
    FLASH_EraseInitTypeDef er = {0};
    uint32_t err = 0;

    /* Bank 2 holds both config sectors; sector index is within the bank. */
    er.TypeErase = FLASH_TYPEERASE_SECTORS;
    er.Banks     = FLASH_BANK_2;
    er.Sector    = (addr - (FLASH_BASE + FLASH_BANK_SIZE)) / NVM_SECTOR_SIZE;
    er.NbSectors = 1u;

    if (HAL_FLASHEx_Erase(&er, &err) != HAL_OK) return false;
    return err == 0xFFFFFFFFu || err == 0u;
}

static bool flash_write_config(uint32_t addr, const evse_config_t *c)
{
    /*
     * The H5 programs flash in 128-bit quad-words, so the payload is copied
     * into a padded staging buffer rather than programmed straight from the
     * struct — sizeof(evse_config_t) is not guaranteed to be a multiple of 16.
     */
    static uint8_t staging[((sizeof(evse_config_t) + 15u) / 16u) * 16u];
    memset(staging, 0xFF, sizeof(staging));
    memcpy(staging, c, sizeof(*c));

    if (HAL_FLASH_Unlock() != HAL_OK) return false;
    bool ok = flash_erase_sector(addr);

    for (size_t off = 0; ok && off < sizeof(staging); off += 16u) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                              addr + off,
                              (uint32_t)(uintptr_t)&staging[off]) != HAL_OK) {
            ok = false;
        }
    }
    HAL_FLASH_Lock();

    return ok && memcmp((const void *)(uintptr_t)addr, c, sizeof(*c)) == 0;
}

void cfg_init(void)
{
    const evse_config_t *a = cfg_at(NVM_CONFIG_ADDR);
    const evse_config_t *b = cfg_at(NVM_CONFIG_ADDR_B);

    bool a_ok = cfg_valid(a);
    bool b_ok = cfg_valid(b);

    if (a_ok && b_ok) {
        /*
         * Sequence numbers wrap eventually; compare as a signed difference so
         * the newer copy is still identified correctly across the wrap.
         */
        bool a_newer = (int32_t)(a->sequence - b->sequence) > 0;
        s_cfg = a_newer ? *a : *b;
        s_active_addr = a_newer ? NVM_CONFIG_ADDR : NVM_CONFIG_ADDR_B;
    } else if (a_ok) {
        s_cfg = *a;
        s_active_addr = NVM_CONFIG_ADDR;
    } else if (b_ok) {
        s_cfg = *b;
        s_active_addr = NVM_CONFIG_ADDR_B;
    } else {
        cfg_load_defaults(&s_cfg);
        s_active_addr = NVM_CONFIG_ADDR_B;   /* so the first save writes A */
    }

    s_cfg.boot_count++;
}

const evse_config_t *cfg_get(void)     { return &s_cfg; }
evse_config_t       *cfg_mutable(void) { return &s_cfg; }

bool cfg_save(void)
{
    /* Always write the copy we are NOT currently running from. */
    uint32_t target = (s_active_addr == NVM_CONFIG_ADDR)
                    ? NVM_CONFIG_ADDR_B : NVM_CONFIG_ADDR;

    s_cfg.magic   = CFG_MAGIC;
    s_cfg.version = CFG_VERSION;
    s_cfg.size    = (uint16_t)sizeof(evse_config_t);
    s_cfg.sequence++;
    s_cfg.crc32   = cfg_crc(&s_cfg);

    if (!flash_write_config(target, &s_cfg)) {
        s_cfg.sequence--;      /* keep RAM consistent with what is on flash */
        return false;
    }
    s_active_addr = target;
    return true;
}

/* ====================================================================== */
/* OCPP configuration keys                                                */
/* ====================================================================== */

typedef enum { KT_BOOL, KT_INT, KT_STR, KT_CSL } key_type_t;

typedef struct {
    const char *name;
    key_type_t  type;
    bool        readonly;
    bool        reboot_required;
    void       *field;
    size_t      field_len;
    int32_t     min, max;      /* for KT_INT */
} ocpp_key_t;

/*
 * The OCPP 1.6 Core profile mandates a specific set of configuration keys.
 * Everything below is either mandatory or genuinely implemented — advertising
 * a key we ignore is worse than reporting NotSupported, because the CSMS then
 * believes it has configured something it has not.
 */
static const ocpp_key_t OCPP_KEYS[] = {
    { "HeartbeatInterval",            KT_INT,  false, false,
      &s_cfg.heartbeat_interval_s,   sizeof(s_cfg.heartbeat_interval_s),   10, 86400 },
    { "MeterValueSampleInterval",     KT_INT,  false, false,
      &s_cfg.meter_value_interval_s, sizeof(s_cfg.meter_value_interval_s),  0,  3600 },
    { "AllowOfflineTxForUnknownId",   KT_BOOL, false, false,
      &s_cfg.allow_offline_charging, sizeof(bool), 0, 0 },
    { "AuthorizeRemoteTxRequests",    KT_BOOL, true,  false, NULL, 0, 0, 0 },
    { "StopTransactionOnEVSideDisconnect", KT_BOOL, false, false,
      &s_cfg.stop_txn_on_ev_disconnect, sizeof(bool), 0, 0 },
    { "FreeVend",                     KT_BOOL, false, false,
      &s_cfg.free_vend, sizeof(bool), 0, 0 },
    { "ConnectorPhaseRotation",       KT_STR,  true,  false, NULL, 0, 0, 0 },
    { "NumberOfConnectors",           KT_INT,  true,  false, NULL, 0, 0, 0 },
    { "SupportedFeatureProfiles",     KT_CSL,  true,  false, NULL, 0, 0, 0 },
    { "MeterValuesSampledData",       KT_CSL,  true,  false, NULL, 0, 0, 0 },
    { "ChargePointVendor",            KT_STR,  true,  false, NULL, 0, 0, 0 },
    { "ChargePointModel",             KT_STR,  true,  false, NULL, 0, 0, 0 },
    { "MaxCurrentA",                  KT_INT,  true,  false, NULL, 0, 0, 0 },
};

#define OCPP_KEY_COUNT (sizeof(OCPP_KEYS)/sizeof(OCPP_KEYS[0]))

static const ocpp_key_t *find_key(const char *name)
{
    for (size_t i = 0; i < OCPP_KEY_COUNT; i++) {
        if (strcmp(OCPP_KEYS[i].name, name) == 0) return &OCPP_KEYS[i];
    }
    return NULL;
}

const char *cfg_ocpp_key_at(size_t index)
{
    return (index < OCPP_KEY_COUNT) ? OCPP_KEYS[index].name : NULL;
}

bool cfg_get_ocpp_key(const char *key, char *out, size_t out_len, bool *readonly)
{
    const ocpp_key_t *k = find_key(key);
    if (k == NULL) return false;
    if (readonly) *readonly = k->readonly;

    /* Read-only keys that report live state rather than a stored field. */
    if (strcmp(key, "AuthorizeRemoteTxRequests") == 0) {
        snprintf(out, out_len, "true");
    } else if (strcmp(key, "ConnectorPhaseRotation") == 0) {
        snprintf(out, out_len, "%d.RST", EVSE_CONNECTOR_ID);
    } else if (strcmp(key, "NumberOfConnectors") == 0) {
        snprintf(out, out_len, "1");
    } else if (strcmp(key, "SupportedFeatureProfiles") == 0) {
        snprintf(out, out_len,
                 "Core,SmartCharging,LocalAuthListManagement,Reservation,RemoteTrigger");
    } else if (strcmp(key, "MeterValuesSampledData") == 0) {
        snprintf(out, out_len,
                 "Energy.Active.Import.Register,Power.Active.Import,"
                 "Current.Import,Voltage,Current.Offered");
    } else if (strcmp(key, "ChargePointVendor") == 0) {
        snprintf(out, out_len, "%s", EVSE_VENDOR_NAME);
    } else if (strcmp(key, "ChargePointModel") == 0) {
        snprintf(out, out_len, "%s", EVSE_MODEL_NAME);
    } else if (strcmp(key, "MaxCurrentA") == 0) {
        snprintf(out, out_len, "%d", (int)cfg_dip_current_a());
    } else if (k->type == KT_BOOL) {
        snprintf(out, out_len, "%s", *(bool *)k->field ? "true" : "false");
    } else if (k->type == KT_INT) {
        uint32_t v = (k->field_len == sizeof(uint16_t))
                   ? *(uint16_t *)k->field : *(uint32_t *)k->field;
        snprintf(out, out_len, "%lu", (unsigned long)v);
    } else {
        snprintf(out, out_len, "%s", (const char *)k->field);
    }
    return true;
}

cfg_key_result_t cfg_set_ocpp_key(const char *key, const char *value)
{
    const ocpp_key_t *k = find_key(key);
    if (k == NULL)  return CFG_KEY_NOT_SUPPORTED;
    if (k->readonly) return CFG_KEY_REJECTED;

    switch (k->type) {
        case KT_BOOL: {
            bool v;
            if      (strcmp(value, "true")  == 0) v = true;
            else if (strcmp(value, "false") == 0) v = false;
            else return CFG_KEY_REJECTED;
            *(bool *)k->field = v;
            break;
        }
        case KT_INT: {
            char *end = NULL;
            long v = strtol(value, &end, 10);
            if (end == value || *end != '\0')      return CFG_KEY_REJECTED;
            if (v < k->min || v > k->max)          return CFG_KEY_REJECTED;
            if (k->field_len == sizeof(uint16_t)) *(uint16_t *)k->field = (uint16_t)v;
            else                                  *(uint32_t *)k->field = (uint32_t)v;
            break;
        }
        default:
            if (strlen(value) >= k->field_len) return CFG_KEY_REJECTED;
            memcpy(k->field, value, strlen(value) + 1u);
            break;
    }

    if (!cfg_save()) return CFG_KEY_REJECTED;
    return k->reboot_required ? CFG_KEY_REBOOT_REQUIRED : CFG_KEY_ACCEPTED;
}
