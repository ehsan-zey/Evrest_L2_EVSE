/**
 * @file  evse_config.h
 * @brief Persistent configuration, stored redundantly in on-chip flash.
 *
 * Two copies (A and B) live in separate flash sectors, each with a CRC and a
 * monotonically increasing sequence number. Writes always target the *older*
 * copy, so an interrupted write can never destroy the only good record. On load
 * the newest copy with a valid CRC wins.
 */
#ifndef EVSE_CONFIG_H
#define EVSE_CONFIG_H

#include "evse_types.h"
#include "safety.h"
#include "proximity.h"
#include <stddef.h>

#define CFG_MAGIC             0x45565231UL   /* "EVR1" */
#define CFG_VERSION           1u

#define CFG_STR_URL_LEN       128
#define CFG_STR_ID_LEN        32
#define CFG_STR_KEY_LEN       64
#define CFG_STR_SSID_LEN      33
#define CFG_STR_PSK_LEN       64

typedef enum {
    NET_IFACE_AUTO = 0,   /**< Ethernet if the link is up, else Wi-Fi */
    NET_IFACE_ETH,
    NET_IFACE_WIFI
} net_iface_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;              /**< sizeof(evse_config_t), forward compat   */
    uint32_t sequence;          /**< higher wins between copies A and B      */

    /* --- Installation ------------------------------------------------- */
    /**
     * Current limit written at commissioning. The DIP switches are the
     * authoritative installed limit; this is only consulted when the DIP code
     * is one of the reserved values, and is always capped by both the DIP
     * setting and EVSE_MAX_CURRENT_A.
     */
    float            max_current_a;
    evse_earthing_t  earthing;
    connector_type_t connector_type;
    bool             ventilation_available;  /**< may we honour pilot state D? */
    uint8_t          phase_count;

    /* --- OCPP --------------------------------------------------------- */
    char     csms_url[CFG_STR_URL_LEN];      /**< ws:// or wss:// endpoint   */
    char     charge_point_id[CFG_STR_ID_LEN];
    char     auth_key[CFG_STR_KEY_LEN];      /**< Basic auth password        */
    uint16_t heartbeat_interval_s;
    uint16_t meter_value_interval_s;
    uint8_t  security_profile;               /**< 0 unsecured .. 3 mutual TLS */
    bool     allow_offline_charging;         /**< charge on cached auth       */
    bool     free_vend;                      /**< no authorisation required   */
    bool     stop_txn_on_ev_disconnect;

    /* --- Network ------------------------------------------------------ */
    net_iface_t iface;
    bool     dhcp;
    uint32_t static_ip;
    uint32_t static_netmask;
    uint32_t static_gateway;
    uint32_t static_dns;
    char     wifi_ssid[CFG_STR_SSID_LEN];
    char     wifi_psk[CFG_STR_PSK_LEN];

    /* --- Calibration, written by the production test rig --------------- */
    int32_t  cp_cal_scale_num;
    int32_t  cp_cal_scale_den;
    int32_t  cp_cal_offset_mv;

    /* --- Runtime state that must survive a reset ----------------------- */
    uint64_t energy_total_wh;
    uint32_t lockout_faults;    /**< latching faults re-applied at boot      */
    uint32_t boot_count;

    uint32_t crc32;             /**< CRC over everything above this field    */
} evse_config_t;

/** Load the newest valid copy, or install defaults if neither is usable. */
void cfg_init(void);

/** Live configuration. Read-only for everything but cfg_save(). */
const evse_config_t *cfg_get(void);

/** Mutable handle for a modification, to be followed by cfg_save(). */
evse_config_t *cfg_mutable(void);

/**
 * Persist the live configuration to the older of the two flash copies.
 * Blocks for a few milliseconds and stalls the flash bus; do not call from a
 * fast periodic task.
 */
bool cfg_save(void);

/** Restore compiled-in defaults in RAM. Does not touch flash. */
void cfg_load_defaults(evse_config_t *cfg);

/* ---- Installer DIP switches ---- */

/** Raw 3-bit DIP code, DIP_1 as bit 0. */
uint8_t cfg_dip_code(void);

/**
 * Installed current limit selected by the DIP switches, amps.
 *
 *   000 -> 16 A   001 -> 24 A   010 -> 32 A   011 -> 40 A   100 -> 48 A
 *   101, 110, 111 -> reserved, fail safe to DIP_RESERVED_CURRENT_A
 *
 * Always capped by EVSE_MAX_CURRENT_A.
 */
float cfg_dip_current_a(void);

/* ---- OCPP configuration keys ---- */

/** Result of ChangeConfiguration, matching the OCPP 1.6 enum. */
typedef enum {
    CFG_KEY_ACCEPTED = 0,
    CFG_KEY_REJECTED,
    CFG_KEY_NOT_SUPPORTED,
    CFG_KEY_REBOOT_REQUIRED
} cfg_key_result_t;

cfg_key_result_t cfg_set_ocpp_key(const char *key, const char *value);

/** Read back an OCPP key. @return true if the key exists. */
bool cfg_get_ocpp_key(const char *key, char *out, size_t out_len, bool *readonly);

/** Iterate the supported OCPP keys; NULL past the end. */
const char *cfg_ocpp_key_at(size_t index);

#endif /* EVSE_CONFIG_H */
