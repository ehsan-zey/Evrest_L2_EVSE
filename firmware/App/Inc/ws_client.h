/**
 * @file  ws_client.h
 * @brief RFC 6455 WebSocket client, the transport OCPP 1.6J runs over.
 *
 * Scope is deliberately narrow — what an OCPP charge point needs and nothing
 * more: a client-side handshake, text frames, ping/pong and close. No
 * extensions, no permessage-deflate, no server role.
 *
 * Frames from a client MUST be masked (RFC 6455 §5.3); a compliant server will
 * close the connection on an unmasked frame, so the mask is not optional and
 * the key comes from a per-connection PRNG rather than a constant.
 */
#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include "net_link.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WS_CLOSED = 0,
    WS_CONNECTING,
    WS_OPEN,
    WS_CLOSING
} ws_state_t;

typedef enum {
    WS_EVT_NONE = 0,
    WS_EVT_TEXT,        /**< a complete text message is available */
    WS_EVT_CLOSED,      /**< the connection went away             */
    WS_EVT_ERROR
} ws_event_t;

typedef struct ws_client ws_client_t;

/**
 * Configure a client. The buffers are caller-owned so the OCPP layer decides
 * how large a message it is willing to handle.
 *
 * @param rx_buf   assembly buffer for inbound messages
 * @param rx_cap   its size; messages larger than this close the connection
 *                 rather than being silently truncated into invalid JSON
 * @param tx_buf   scratch for outbound framing, needs rx_cap/2 + 16 or so
 */
ws_client_t *ws_create(uint8_t *rx_buf, size_t rx_cap, uint8_t *tx_buf, size_t tx_cap);

/**
 * Parse @p url ("ws://host:port/path" or "wss://...") and perform the opening
 * handshake, requesting the "ocpp1.6" subprotocol.
 *
 * @param basic_auth_user  charge point id, or NULL for no Authorization header
 * @param basic_auth_pass  the pre-shared key (OCPP security profile 1/2)
 */
bool ws_connect(ws_client_t *ws, const char *url,
                const char *subprotocol,
                const char *basic_auth_user, const char *basic_auth_pass);

/** Send one text message. */
bool ws_send_text(ws_client_t *ws, const char *text, size_t len);

/**
 * Service the connection: read frames, answer pings, assemble fragments.
 *
 * @param out_msg  set to the assembled message on WS_EVT_TEXT (NUL-terminated,
 *                 pointing into the rx buffer and valid until the next call)
 * @param out_len  its length
 */
ws_event_t ws_poll(ws_client_t *ws, uint32_t timeout_ms,
                   const char **out_msg, size_t *out_len);

/** Send a close frame and tear down. */
void ws_close(ws_client_t *ws, uint16_t code);

ws_state_t ws_get_state(const ws_client_t *ws);

/**
 * Send an unsolicited ping. OCPP relies on WebSocket ping/pong to notice a
 * connection that has gone away without a FIN, which on a mobile-backhauled
 * site is the common case.
 */
bool ws_ping(ws_client_t *ws);

/** Milliseconds since the last frame of any kind arrived. */
uint32_t ws_idle_ms(const ws_client_t *ws);

/* ---- Exposed for unit testing ---- */

/** RFC 6455 §1.3 handshake accept value: base64(sha1(key + GUID)). */
void ws_compute_accept(const char *client_key, char *out, size_t out_len);
void ws_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len);

/**
 * Split a ws:// or wss:// URL.
 * @return false if the scheme is missing or unrecognised.
 */
bool ws_parse_url(const char *url, char *host, size_t host_len,
                  uint16_t *port, char *path, size_t path_len, bool *tls);

#endif /* WS_CLIENT_H */
