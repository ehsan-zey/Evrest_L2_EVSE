/**
 * @file  ws_client.c
 * @brief RFC 6455 WebSocket client. See ws_client.h.
 */
#include "ws_client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef EVSE_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
static void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#else
#include <time.h>
static uint32_t now_ms(void) { return (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC); }
static void sleep_ms(uint32_t ms) { (void)ms; }
#endif

/* ====================================================================== */
/* SHA-1 and base64                                                       */
/*                                                                        */
/* Built in rather than pulled from mbedTLS: the handshake has to work on  */
/* the Wi-Fi path too, where TLS is terminated on the ESP32 and mbedTLS    */
/* may not be linked at all. Both are small and neither is used for        */
/* anything security-critical — the accept value is an integrity check,    */
/* not authentication.                                                     */
/* ====================================================================== */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx_t;

static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i*4] << 24) | ((uint32_t)buffer[i*4+1] << 16) |
               ((uint32_t)buffer[i*4+2] << 8) | (uint32_t)buffer[i*4+3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301u; ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu; ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->count = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t idx = (size_t)((ctx->count / 8) % 64);
    ctx->count += (uint64_t)len * 8u;

    size_t i = 0;
    if (idx + len >= 64) {
        size_t fill = 64 - idx;
        memcpy(&ctx->buffer[idx], data, fill);
        sha1_transform(ctx->state, ctx->buffer);
        for (i = fill; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        idx = 0;
    }
    memcpy(&ctx->buffer[idx], &data[i], len - i);
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20])
{
    uint8_t finalcount[8];
    for (int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)(ctx->count >> ((7 - i) * 8));
    }
    uint8_t c = 0x80;
    sha1_update(ctx, &c, 1);
    c = 0x00;
    while ((ctx->count / 8) % 64 != 56) {
        sha1_update(ctx, &c, 1);
    }
    sha1_update(ctx, finalcount, 8);

    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)(ctx->state[i >> 2] >> ((3 - (i & 3)) * 8));
    }
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void ws_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i+2];

        if (o + 4 >= out_len) break;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? B64[v & 0x3F]        : '=';
    }
    out[o] = '\0';
}

/** The GUID RFC 6455 §1.3 mandates be appended to the client key. */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void ws_compute_accept(const char *client_key, char *out, size_t out_len)
{
    sha1_ctx_t ctx;
    uint8_t digest[20];

    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)client_key, strlen(client_key));
    sha1_update(&ctx, (const uint8_t *)WS_GUID, strlen(WS_GUID));
    sha1_final(&ctx, digest);

    ws_base64_encode(digest, sizeof(digest), out, out_len);
}

/* ====================================================================== */
/* URL parsing                                                            */
/* ====================================================================== */

bool ws_parse_url(const char *url, char *host, size_t host_len,
                  uint16_t *port, char *path, size_t path_len, bool *tls)
{
    if (url == NULL) return false;

    const char *p;
    if (strncmp(url, "wss://", 6) == 0)     { *tls = true;  *port = 443u; p = url + 6; }
    else if (strncmp(url, "ws://", 5) == 0) { *tls = false; *port = 80u;  p = url + 5; }
    else return false;

    /* Host runs to the first ':' or '/'. */
    size_t h = 0;
    while (*p && *p != ':' && *p != '/' && h + 1 < host_len) {
        host[h++] = *p++;
    }
    host[h] = '\0';
    if (h == 0) return false;
    /* Skip any host characters that did not fit rather than splicing them
     * into the port or path. */
    while (*p && *p != ':' && *p != '/') p++;

    if (*p == ':') {
        p++;
        long v = strtol(p, (char **)&p, 10);
        if (v <= 0 || v > 65535) return false;
        *port = (uint16_t)v;
    }

    if (*p == '\0') {
        snprintf(path, path_len, "/");
    } else {
        snprintf(path, path_len, "%s", p);
    }
    return true;
}

/* ====================================================================== */
/* Client                                                                 */
/* ====================================================================== */

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

struct ws_client {
    net_sock_t sock;
    ws_state_t state;

    uint8_t *rx;          /**< assembled message                        */
    size_t   rx_cap;
    size_t   rx_len;      /**< bytes of message assembled so far        */

    uint8_t *frame;       /**< raw bytes not yet consumed as frames     */
    size_t   frame_cap;
    size_t   frame_len;

    uint8_t *tx;
    size_t   tx_cap;

    uint32_t last_rx_ms;
    uint32_t mask_state;  /**< xorshift PRNG state for frame masking    */
    bool     fragment_is_text;
};

static struct ws_client s_client;   /* one OCPP session, one client */

ws_client_t *ws_create(uint8_t *rx_buf, size_t rx_cap, uint8_t *tx_buf, size_t tx_cap)
{
    memset(&s_client, 0, sizeof(s_client));
    s_client.sock   = NET_SOCK_INVALID;
    s_client.state  = WS_CLOSED;
    s_client.rx     = rx_buf;
    s_client.rx_cap = rx_cap;
    s_client.tx     = tx_buf;
    s_client.tx_cap = tx_cap;

    /*
     * The raw-frame staging area lives in the back half of the tx buffer.
     * Inbound frames are consumed and copied into rx as they complete, so the
     * two never need the same bytes at the same time.
     */
    s_client.frame     = tx_buf + tx_cap / 2u;
    s_client.frame_cap = tx_cap / 2u;
    s_client.tx_cap    = tx_cap / 2u;
    return &s_client;
}

/**
 * xorshift32 for the frame mask.
 *
 * The mask exists to stop intermediaries being confused by attacker-chosen
 * payloads, not to provide secrecy, so a PRNG is adequate. It is seeded from
 * the clock plus the socket handle so two chargers booting together do not
 * produce identical mask sequences.
 */
static uint32_t next_mask(ws_client_t *ws)
{
    uint32_t x = ws->mask_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ws->mask_state = x;
    return x;
}

static bool send_frame(ws_client_t *ws, uint8_t opcode,
                       const uint8_t *payload, size_t len)
{
    if (ws->sock == NET_SOCK_INVALID) return false;

    uint8_t hdr[14];
    size_t h = 0;

    hdr[h++] = (uint8_t)(0x80u | opcode);          /* FIN + opcode */

    /* Bit 7 of the second byte is the MASK bit; clients must always set it. */
    if (len < 126u) {
        hdr[h++] = (uint8_t)(0x80u | len);
    } else if (len < 65536u) {
        hdr[h++] = 0x80u | 126u;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFFu);
    } else {
        hdr[h++] = 0x80u | 127u;
        for (int i = 7; i >= 0; i--) {
            hdr[h++] = (uint8_t)((uint64_t)len >> (i * 8));
        }
    }

    uint32_t mask = next_mask(ws);
    uint8_t mkey[4] = { (uint8_t)(mask >> 24), (uint8_t)(mask >> 16),
                        (uint8_t)(mask >> 8),  (uint8_t)mask };
    memcpy(&hdr[h], mkey, 4);
    h += 4;

    if (net_send(ws->sock, hdr, h, 5000) != NET_OK) return false;

    /* Mask in place through the tx scratch buffer, a chunk at a time, so a
     * large message does not need a second full-size buffer. */
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > ws->tx_cap) chunk = ws->tx_cap;
        for (size_t i = 0; i < chunk; i++) {
            ws->tx[i] = payload[off + i] ^ mkey[(off + i) & 3u];
        }
        if (net_send(ws->sock, ws->tx, chunk, 5000) != NET_OK) return false;
        off += chunk;
    }
    return true;
}

bool ws_connect(ws_client_t *ws, const char *url, const char *subprotocol,
                const char *basic_auth_user, const char *basic_auth_pass)
{
    char host[96], path[128];
    uint16_t port;
    bool tls;

    if (!ws_parse_url(url, host, sizeof(host), &port, path, sizeof(path), &tls)) {
        return false;
    }

    ws->state = WS_CONNECTING;
    ws->sock = net_connect(host, port, tls, 10000);
    if (ws->sock == NET_SOCK_INVALID) {
        ws->state = WS_CLOSED;
        return false;
    }

    ws->mask_state = now_ms() ^ ((uint32_t)ws->sock << 16) ^ 0xA5A5A5A5u;
    if (ws->mask_state == 0u) ws->mask_state = 0x1234567u;   /* xorshift dies at 0 */

    /* 16 random bytes, base64'd, per RFC 6455 §4.1. */
    uint8_t keybytes[16];
    for (int i = 0; i < 16; i++) keybytes[i] = (uint8_t)(next_mask(ws) >> 24);
    char key_b64[32];
    ws_base64_encode(keybytes, sizeof(keybytes), key_b64, sizeof(key_b64));

    char req[640];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: %s\r\n",
        path, host, port, key_b64, subprotocol ? subprotocol : "ocpp1.6");

    if (basic_auth_user && basic_auth_user[0] && basic_auth_pass && basic_auth_pass[0]) {
        char creds[160];
        int cn = snprintf(creds, sizeof(creds), "%s:%s", basic_auth_user, basic_auth_pass);
        char creds_b64[224];
        ws_base64_encode((const uint8_t *)creds, (size_t)cn, creds_b64, sizeof(creds_b64));
        n += snprintf(req + n, sizeof(req) - (size_t)n,
                      "Authorization: Basic %s\r\n", creds_b64);
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");

    if (net_send(ws->sock, req, (size_t)n, 5000) != NET_OK) goto fail;

    /* Read the response headers up to the blank line. */
    char resp[768];
    size_t total = 0;
    uint32_t deadline = now_ms() + 10000u;
    bool complete = false;

    while (!complete && total + 1 < sizeof(resp)) {
        if ((int32_t)(now_ms() - deadline) >= 0) goto fail;
        size_t got = 0;
        net_err_t r = net_recv(ws->sock, resp + total, sizeof(resp) - 1 - total,
                               &got, 1000);
        if (r == NET_ERR_CLOSED) goto fail;
        if (got == 0) continue;
        total += got;
        resp[total] = '\0';
        complete = (strstr(resp, "\r\n\r\n") != NULL);
    }
    if (!complete) goto fail;

    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        /* 401 here almost always means the auth key is wrong, which is worth
         * distinguishing from a network failure when someone is on site. */
        printf("ws: handshake rejected: %.32s\r\n", resp);
        goto fail;
    }

    /*
     * Verify Sec-WebSocket-Accept. A server that echoes the wrong value is not
     * speaking WebSocket, and continuing would mean framing arbitrary bytes.
     */
    char expect[32];
    ws_compute_accept(key_b64, expect, sizeof(expect));
    const char *acc = strstr(resp, "Sec-WebSocket-Accept:");
    if (acc == NULL) goto fail;
    acc += strlen("Sec-WebSocket-Accept:");
    while (*acc == ' ') acc++;
    if (strncmp(acc, expect, strlen(expect)) != 0) {
        printf("ws: accept mismatch\r\n");
        goto fail;
    }

    ws->state      = WS_OPEN;
    ws->rx_len     = 0;
    ws->frame_len  = 0;
    ws->last_rx_ms = now_ms();
    return true;

fail:
    net_close(ws->sock);
    ws->sock  = NET_SOCK_INVALID;
    ws->state = WS_CLOSED;
    return false;
}

bool ws_send_text(ws_client_t *ws, const char *text, size_t len)
{
    if (ws->state != WS_OPEN) return false;
    return send_frame(ws, WS_OP_TEXT, (const uint8_t *)text, len);
}

bool ws_ping(ws_client_t *ws)
{
    if (ws->state != WS_OPEN) return false;
    return send_frame(ws, WS_OP_PING, NULL, 0);
}

uint32_t ws_idle_ms(const ws_client_t *ws) { return now_ms() - ws->last_rx_ms; }
ws_state_t ws_get_state(const ws_client_t *ws) { return ws->state; }

/** Drop @p n bytes from the front of the raw frame buffer. */
static void frame_consume(ws_client_t *ws, size_t n)
{
    if (n >= ws->frame_len) { ws->frame_len = 0; return; }
    memmove(ws->frame, ws->frame + n, ws->frame_len - n);
    ws->frame_len -= n;
}

/**
 * Try to parse one complete frame out of the raw buffer.
 * @return true if a frame was consumed; *event is set if it produced one.
 */
static bool parse_one_frame(ws_client_t *ws, ws_event_t *event,
                            const char **out_msg, size_t *out_len)
{
    if (ws->frame_len < 2u) return false;

    const uint8_t *f = ws->frame;
    bool     fin    = (f[0] & 0x80u) != 0u;
    uint8_t  opcode =  f[0] & 0x0Fu;
    bool     masked = (f[1] & 0x80u) != 0u;
    uint64_t len    =  f[1] & 0x7Fu;
    size_t   hdr    = 2u;

    if (len == 126u) {
        if (ws->frame_len < 4u) return false;
        len = ((uint64_t)f[2] << 8) | f[3];
        hdr = 4u;
    } else if (len == 127u) {
        if (ws->frame_len < 10u) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | f[2 + i];
        hdr = 10u;
    }
    /* A server must not mask, but handle it rather than mis-framing. */
    uint8_t mkey[4] = {0};
    if (masked) {
        if (ws->frame_len < hdr + 4u) return false;
        memcpy(mkey, f + hdr, 4);
        hdr += 4u;
    }

    if (ws->frame_len < hdr + len) return false;    /* still incomplete */

    const uint8_t *payload = f + hdr;

    switch (opcode) {
    case WS_OP_PING:
        /* Echo the payload back, as RFC 6455 §5.5.3 requires. */
        send_frame(ws, WS_OP_PONG, payload, (size_t)len);
        break;

    case WS_OP_PONG:
        break;                                      /* liveness only */

    case WS_OP_CLOSE:
        ws->state = WS_CLOSING;
        send_frame(ws, WS_OP_CLOSE, payload, (size_t)(len > 125u ? 125u : len));
        *event = WS_EVT_CLOSED;
        break;

    case WS_OP_TEXT:
    case WS_OP_BIN:
        ws->rx_len = 0;
        ws->fragment_is_text = (opcode == WS_OP_TEXT);
        /* fall through */
    case WS_OP_CONT:
        if (ws->rx_len + len + 1u > ws->rx_cap) {
            /*
             * Refuse rather than truncate. A truncated OCPP message is invalid
             * JSON that the client would reject anyway, but silently — this
             * way the cause is visible.
             */
            printf("ws: message exceeds %u byte buffer, closing\r\n",
                   (unsigned)ws->rx_cap);
            ws_close(ws, 1009);                     /* 1009 = message too big */
            *event = WS_EVT_CLOSED;
            break;
        }
        if (masked) {
            for (uint64_t i = 0; i < len; i++) {
                ws->rx[ws->rx_len + i] = payload[i] ^ mkey[i & 3u];
            }
        } else {
            memcpy(ws->rx + ws->rx_len, payload, (size_t)len);
        }
        ws->rx_len += (size_t)len;

        if (fin && ws->fragment_is_text) {
            ws->rx[ws->rx_len] = '\0';
            *out_msg = (const char *)ws->rx;
            *out_len = ws->rx_len;
            *event   = WS_EVT_TEXT;
        }
        break;

    default:
        break;                                      /* reserved: ignore */
    }

    frame_consume(ws, hdr + (size_t)len);
    ws->last_rx_ms = now_ms();
    return true;
}

ws_event_t ws_poll(ws_client_t *ws, uint32_t timeout_ms,
                   const char **out_msg, size_t *out_len)
{
    if (ws->state != WS_OPEN && ws->state != WS_CLOSING) return WS_EVT_CLOSED;

    ws_event_t event = WS_EVT_NONE;

    /* Anything already buffered may complete a frame without a read. */
    while (parse_one_frame(ws, &event, out_msg, out_len)) {
        if (event != WS_EVT_NONE) return event;
    }

    if (ws->frame_len >= ws->frame_cap) {
        /* Buffer full and still no complete frame: the peer is sending
         * something we cannot assemble. */
        ws_close(ws, 1009);
        return WS_EVT_CLOSED;
    }

    size_t got = 0;
    net_err_t r = net_recv(ws->sock, ws->frame + ws->frame_len,
                           ws->frame_cap - ws->frame_len, &got, timeout_ms);
    if (r == NET_ERR_CLOSED) {
        ws->state = WS_CLOSED;
        net_close(ws->sock);
        ws->sock = NET_SOCK_INVALID;
        return WS_EVT_CLOSED;
    }
    if (got == 0) {
        sleep_ms(1);
        return WS_EVT_NONE;
    }

    ws->frame_len += got;
    ws->last_rx_ms = now_ms();

    while (parse_one_frame(ws, &event, out_msg, out_len)) {
        if (event != WS_EVT_NONE) return event;
    }
    return WS_EVT_NONE;
}

void ws_close(ws_client_t *ws, uint16_t code)
{
    if (ws->sock == NET_SOCK_INVALID) { ws->state = WS_CLOSED; return; }

    if (ws->state == WS_OPEN) {
        uint8_t body[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFFu) };
        send_frame(ws, WS_OP_CLOSE, body, sizeof(body));
    }
    net_close(ws->sock);
    ws->sock  = NET_SOCK_INVALID;
    ws->state = WS_CLOSED;
    ws->rx_len = 0;
    ws->frame_len = 0;
}
