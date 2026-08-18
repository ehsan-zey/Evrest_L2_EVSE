/**
 * @file  net_link.c
 * @brief Transport over Ethernet (LwIP) and Wi-Fi (ESP32 AT). See net_link.h.
 */
#include "net_link.h"
#include "evse_board.h"
#include "esp_at.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

#if EVSE_USE_LWIP
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
extern struct netif gnetif;
#endif

/**
 * Socket handles are tagged so one API can address both backends.
 * Ethernet sockets are returned as-is (LwIP descriptors are small
 * non-negative integers); ESP32 link ids are offset so the two spaces cannot
 * collide and a handle from one backend can never be passed to the other.
 */
#define WIFI_SOCK_BASE   0x1000

static const evse_config_t *s_cfg;
static net_active_t         s_active;
static uint32_t             s_last_poll_ms;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* ====================================================================== */
/* Ethernet backend                                                       */
/* ====================================================================== */

#if EVSE_USE_LWIP

/*
 * One mbedTLS context per socket. A charger holds exactly one outbound
 * connection — the OCPP session — plus at most one transient one for a
 * firmware download, so two is enough and static allocation avoids a heap.
 */
#define TLS_SLOT_COUNT 2

typedef struct {
    bool                     in_use;
    int                      fd;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_net_context      net;
} tls_slot_t;

static tls_slot_t              s_tls[TLS_SLOT_COUNT];
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_x509_crt        s_ca;
static bool                    s_tls_ready;

/** CA bundle used to verify the CSMS. Provisioned at manufacture. */
extern const unsigned char evse_ca_bundle_pem[];
extern const size_t        evse_ca_bundle_pem_len;

static bool tls_global_init(void)
{
    if (s_tls_ready) return true;

    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_x509_crt_init(&s_ca);

    static const char *pers = "evrest-evse";
    if (mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        return false;
    }
    if (mbedtls_x509_crt_parse(&s_ca, evse_ca_bundle_pem,
                               evse_ca_bundle_pem_len) != 0) {
        return false;
    }
    s_tls_ready = true;
    return true;
}

static tls_slot_t *tls_slot_alloc(void)
{
    for (int i = 0; i < TLS_SLOT_COUNT; i++) {
        if (!s_tls[i].in_use) { s_tls[i].in_use = true; return &s_tls[i]; }
    }
    return NULL;
}

static tls_slot_t *tls_slot_for_fd(int fd)
{
    for (int i = 0; i < TLS_SLOT_COUNT; i++) {
        if (s_tls[i].in_use && s_tls[i].fd == fd) return &s_tls[i];
    }
    return NULL;
}

static net_sock_t eth_connect(const char *host, uint16_t port, bool tls, uint32_t timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return NET_SOCK_INVALID;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return NET_SOCK_INVALID; }

    struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000u),
                          .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u) };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) { close(fd); return NET_SOCK_INVALID; }

    if (!tls) {
        return (net_sock_t)fd;
    }

    if (!tls_global_init()) { close(fd); return NET_SOCK_INVALID; }
    tls_slot_t *slot = tls_slot_alloc();
    if (slot == NULL) { close(fd); return NET_SOCK_INVALID; }

    slot->fd = fd;
    slot->net.fd = fd;
    mbedtls_ssl_init(&slot->ssl);
    mbedtls_ssl_config_init(&slot->conf);

    if (mbedtls_ssl_config_defaults(&slot->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        goto fail;
    }
    /*
     * Certificate verification is REQUIRED, not optional. An OCPP session
     * carries authorisation decisions and meter readings; accepting any
     * certificate would let anything on the network start and stop charging.
     */
    mbedtls_ssl_conf_authmode(&slot->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&slot->conf, &s_ca, NULL);
    mbedtls_ssl_conf_rng(&slot->conf, mbedtls_ctr_drbg_random, &s_drbg);

    if (mbedtls_ssl_setup(&slot->ssl, &slot->conf) != 0)          goto fail;
    /* SNI, and the name checked against the certificate. */
    if (mbedtls_ssl_set_hostname(&slot->ssl, host) != 0)          goto fail;
    mbedtls_ssl_set_bio(&slot->ssl, &slot->net,
                        mbedtls_net_send, mbedtls_net_recv, NULL);

    for (;;) {
        int r = mbedtls_ssl_handshake(&slot->ssl);
        if (r == 0) break;
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            goto fail;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (mbedtls_ssl_get_verify_result(&slot->ssl) != 0) goto fail;

    return (net_sock_t)fd;

fail:
    mbedtls_ssl_free(&slot->ssl);
    mbedtls_ssl_config_free(&slot->conf);
    slot->in_use = false;
    close(fd);
    return NET_SOCK_INVALID;
}

static net_err_t eth_send(int fd, const void *data, size_t len, uint32_t timeout_ms)
{
    tls_slot_t *slot = tls_slot_for_fd(fd);
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    uint32_t deadline = now_ms() + timeout_ms;

    while (sent < len) {
        int r;
        if (slot) {
            r = mbedtls_ssl_write(&slot->ssl, p + sent, len - sent);
            if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) r = 0;
        } else {
            r = (int)send(fd, p + sent, len - sent, 0);
        }
        if (r > 0) {
            sent += (size_t)r;
            continue;
        }
        if (r < 0) return NET_ERR_CLOSED;
        if ((int32_t)(now_ms() - deadline) >= 0) return NET_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return NET_OK;
}

static net_err_t eth_recv(int fd, void *buf, size_t len, size_t *received,
                          uint32_t timeout_ms)
{
    tls_slot_t *slot = tls_slot_for_fd(fd);
    *received = 0;

    int r;
    if (slot) {
        r = mbedtls_ssl_read(&slot->ssl, (unsigned char *)buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(timeout_ms > 20u ? 20u : timeout_ms));
            return NET_ERR_TIMEOUT;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return NET_ERR_CLOSED;
    } else {
        struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000u),
                              .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u) };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        r = (int)recv(fd, buf, len, 0);
    }

    if (r > 0) { *received = (size_t)r; return NET_OK; }
    if (r == 0) return NET_ERR_CLOSED;
    return NET_ERR_TIMEOUT;   /* a timeout on an idle socket is normal */
}

static void eth_close(int fd)
{
    tls_slot_t *slot = tls_slot_for_fd(fd);
    if (slot) {
        mbedtls_ssl_close_notify(&slot->ssl);
        mbedtls_ssl_free(&slot->ssl);
        mbedtls_ssl_config_free(&slot->conf);
        slot->in_use = false;
    }
    close(fd);
}

static bool eth_link_up(void)
{
    return netif_is_up(&gnetif) && netif_is_link_up(&gnetif) &&
           !ip4_addr_isany_val(*netif_ip4_addr(&gnetif));
}

#else  /* !EVSE_USE_LWIP — Wi-Fi only build */

static net_sock_t eth_connect(const char *h, uint16_t p, bool t, uint32_t to)
{ (void)h; (void)p; (void)t; (void)to; return NET_SOCK_INVALID; }
static net_err_t eth_send(int f, const void *d, size_t l, uint32_t t)
{ (void)f; (void)d; (void)l; (void)t; return NET_ERR_NO_LINK; }
static net_err_t eth_recv(int f, void *b, size_t l, size_t *r, uint32_t t)
{ (void)f; (void)b; (void)l; (void)t; *r = 0; return NET_ERR_NO_LINK; }
static void eth_close(int f) { (void)f; }
static bool eth_link_up(void) { return false; }

#endif

/* ====================================================================== */
/* Public API — dispatch on the handle tag                                */
/* ====================================================================== */

bool net_link_init(const evse_config_t *cfg)
{
    s_cfg = cfg;
    s_active = NET_ACTIVE_NONE;

    if (cfg->iface != NET_IFACE_ETH) {
        if (!esp_at_init(cfg->wifi_ssid, cfg->wifi_psk)) {
            /* Not fatal: Ethernet may still come up. */
            printf("net: ESP32 init failed\r\n");
        }
    }
    return true;
}

void net_link_poll(void)
{
    if (now_ms() - s_last_poll_ms < 1000u) return;
    s_last_poll_ms = now_ms();

    bool eth_ok  = (s_cfg->iface != NET_IFACE_WIFI) && eth_link_up();
    bool wifi_ok = (s_cfg->iface != NET_IFACE_ETH)  && esp_at_is_connected();

    /*
     * Ethernet wins when both are available: it is not sharing the enclosure's
     * RF environment with a vehicle, and it does not depend on a co-processor
     * that can itself need rebooting.
     */
    net_active_t want = eth_ok  ? NET_ACTIVE_ETH
                      : wifi_ok ? NET_ACTIVE_WIFI
                                : NET_ACTIVE_NONE;

    if (want != s_active) {
        printf("net: interface -> %s\r\n",
               want == NET_ACTIVE_ETH  ? "ethernet" :
               want == NET_ACTIVE_WIFI ? "wifi" : "none");
        s_active = want;
    }

    if (!wifi_ok && s_cfg->iface != NET_IFACE_ETH) {
        esp_at_service();      /* drives reconnection */
    }
}

bool         net_link_is_up(void)  { return s_active != NET_ACTIVE_NONE; }
net_active_t net_link_active(void) { return s_active; }
int8_t       net_link_rssi(void)
{
    return (s_active == NET_ACTIVE_WIFI) ? esp_at_rssi() : 0;
}

void net_link_address(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    out[0] = '\0';
#if EVSE_USE_LWIP
    if (s_active == NET_ACTIVE_ETH) {
        const ip4_addr_t *ip = netif_ip4_addr(&gnetif);
        snprintf(out, out_len, "%u.%u.%u.%u",
                 ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
        return;
    }
#endif
    if (s_active == NET_ACTIVE_WIFI) {
        esp_at_get_ip(out, out_len);
    }
}

net_sock_t net_connect(const char *host, uint16_t port, bool tls, uint32_t timeout_ms)
{
    if (s_active == NET_ACTIVE_ETH) {
        return eth_connect(host, port, tls, timeout_ms);
    }
    if (s_active == NET_ACTIVE_WIFI) {
        int link = esp_at_connect(host, port, tls, timeout_ms);
        return (link < 0) ? NET_SOCK_INVALID : (net_sock_t)(WIFI_SOCK_BASE + link);
    }
    return NET_SOCK_INVALID;
}

net_err_t net_send(net_sock_t sock, const void *data, size_t len, uint32_t timeout_ms)
{
    if (sock == NET_SOCK_INVALID) return NET_ERR_INTERNAL;
    if (sock >= WIFI_SOCK_BASE) {
        return esp_at_send((int)(sock - WIFI_SOCK_BASE), data, len, timeout_ms);
    }
    return eth_send((int)sock, data, len, timeout_ms);
}

net_err_t net_recv(net_sock_t sock, void *buf, size_t len, size_t *received,
                   uint32_t timeout_ms)
{
    if (received) *received = 0;
    if (sock == NET_SOCK_INVALID) return NET_ERR_INTERNAL;
    if (sock >= WIFI_SOCK_BASE) {
        return esp_at_recv((int)(sock - WIFI_SOCK_BASE), buf, len, received, timeout_ms);
    }
    return eth_recv((int)sock, buf, len, received, timeout_ms);
}

void net_close(net_sock_t sock)
{
    if (sock == NET_SOCK_INVALID) return;
    if (sock >= WIFI_SOCK_BASE) {
        esp_at_close((int)(sock - WIFI_SOCK_BASE));
        return;
    }
    eth_close((int)sock);
}
