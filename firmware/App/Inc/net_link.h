/**
 * @file  net_link.h
 * @brief Network transport abstraction over Ethernet (LwIP) and Wi-Fi (ESP32).
 *
 * The board has both a PHY on RMII and an ESP32 co-processor on USART1. The
 * WebSocket and OCPP layers above this do not care which is carrying traffic,
 * so both are hidden behind one blocking-with-timeout socket API.
 *
 * TLS is handled differently by the two paths, deliberately:
 *   - Ethernet:  mbedTLS runs on the MCU, using the H5's crypto accelerator.
 *   - Wi-Fi:     the ESP32 terminates TLS itself (AT+CIPSTARTs "SSL"), which
 *                keeps a second TLS stack and its certificate store off the
 *                MCU entirely.
 * Either way `net_connect(..., tls=true)` gives an encrypted stream.
 */
#ifndef NET_LINK_H
#define NET_LINK_H

#include "evse_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int32_t net_sock_t;
#define NET_SOCK_INVALID  (-1)

typedef enum {
    NET_OK = 0,
    NET_ERR_TIMEOUT   = -1,
    NET_ERR_CLOSED    = -2,   /**< peer closed cleanly                  */
    NET_ERR_REFUSED   = -3,
    NET_ERR_DNS       = -4,
    NET_ERR_TLS       = -5,
    NET_ERR_NO_LINK   = -6,   /**< no interface is up                   */
    NET_ERR_INTERNAL  = -7
} net_err_t;

/** Which interface is actually carrying traffic right now. */
typedef enum {
    NET_ACTIVE_NONE = 0,
    NET_ACTIVE_ETH,
    NET_ACTIVE_WIFI
} net_active_t;

/**
 * Bring up the interfaces named by @p cfg. Non-blocking; link state is polled
 * by net_link_poll(). Ethernet is preferred in NET_IFACE_AUTO because it does
 * not share the enclosure's RF environment with the vehicle.
 */
bool net_link_init(const evse_config_t *cfg);

/** Service link state, DHCP and the ESP32 link. Call about once a second. */
void net_link_poll(void);

/** True when an interface has a usable IP address. */
bool net_link_is_up(void);

net_active_t net_link_active(void);

/** Dotted-quad of the current address, for the console and StatusNotification. */
void net_link_address(char *out, size_t out_len);

/** Signal strength in dBm, Wi-Fi only. Returns 0 on Ethernet. */
int8_t net_link_rssi(void);

/**
 * Open a TCP (or TLS) connection.
 * @param host     hostname or dotted quad
 * @param port     TCP port
 * @param tls      wrap the stream in TLS
 * @param timeout_ms  how long to wait for the connection to complete
 * @return a socket, or NET_SOCK_INVALID.
 */
net_sock_t net_connect(const char *host, uint16_t port, bool tls, uint32_t timeout_ms);

/**
 * Send exactly @p len bytes, or fail.
 * @return NET_OK, or a negative net_err_t. Partial sends are retried
 *         internally, so a caller never has to track a write offset.
 */
net_err_t net_send(net_sock_t sock, const void *data, size_t len, uint32_t timeout_ms);

/**
 * Read up to @p len bytes.
 * @param received  set to the number of bytes actually read
 * @return NET_OK if at least one byte arrived, NET_ERR_TIMEOUT if none did
 *         within the timeout (which is normal and not an error), or another
 *         negative net_err_t on a real failure.
 */
net_err_t net_recv(net_sock_t sock, void *buf, size_t len, size_t *received,
                   uint32_t timeout_ms);

void net_close(net_sock_t sock);

#endif /* NET_LINK_H */
