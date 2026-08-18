/**
 * Host-side tests for the WebSocket client.
 *
 * A fake server runs interleaved with the client through a stub net layer: the
 * recv stub pumps the server whenever the client has nothing to read, which
 * makes the request/response exchange work without threads. Masking,
 * fragmentation, ping/pong and oversize handling are therefore exercised for
 * real rather than inspected by eye.
 */
#include "ws_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)

/* ====================================================================== */
/* Stub net layer                                                         */
/* ====================================================================== */

#define PIPE_CAP 65536
typedef struct { uint8_t buf[PIPE_CAP]; size_t head, tail; } pipe_t;

static pipe_t g_to_server;
static pipe_t g_to_client;
static bool   g_sock_open;

static void   pipe_write(pipe_t *p, const void *d, size_t n)
{
    const uint8_t *s = d;
    for (size_t i = 0; i < n && p->head < PIPE_CAP; i++) p->buf[p->head++] = s[i];
}
static size_t pipe_read(pipe_t *p, void *d, size_t n)
{
    uint8_t *o = d; size_t i = 0;
    while (i < n && p->tail < p->head) o[i++] = p->buf[p->tail++];
    return i;
}
static size_t pipe_avail(const pipe_t *p) { return p->head - p->tail; }
static void   pipe_reset(pipe_t *p) { p->head = p->tail = 0; }

static void server_pump(void);

net_sock_t net_connect(const char *host, uint16_t port, bool tls, uint32_t timeout_ms)
{
    (void)host; (void)port; (void)tls; (void)timeout_ms;
    g_sock_open = true;
    return 1;
}
net_err_t net_send(net_sock_t s, const void *data, size_t len, uint32_t timeout_ms)
{
    (void)s; (void)timeout_ms;
    if (!g_sock_open) return NET_ERR_CLOSED;
    pipe_write(&g_to_server, data, len);
    return NET_OK;
}
net_err_t net_recv(net_sock_t s, void *buf, size_t len, size_t *received, uint32_t to)
{
    (void)s; (void)to;
    if (!g_sock_open) return NET_ERR_CLOSED;
    if (pipe_avail(&g_to_client) == 0) {
        server_pump();                 /* let the server produce a reply */
    }
    *received = pipe_read(&g_to_client, buf, len);
    return (*received > 0) ? NET_OK : NET_ERR_TIMEOUT;
}
void net_close(net_sock_t s) { (void)s; g_sock_open = false; }

/* ====================================================================== */
/* Fake server                                                            */
/* ====================================================================== */

static bool g_handshaken;
/** Last application message the server received from the client. */
static char   g_server_rx[4096];
static size_t g_server_rx_len;
static int    g_server_pongs;
static int    g_server_closes;
/** Set true if the client ever sent an unmasked frame (a protocol violation). */
static bool   g_saw_unmasked;

static void server_send_frame(uint8_t opcode, const void *payload, size_t len, bool fin)
{
    uint8_t hdr[10];
    size_t h = 0;
    hdr[h++] = (uint8_t)((fin ? 0x80u : 0x00u) | opcode);
    if (len < 126)        { hdr[h++] = (uint8_t)len; }
    else if (len < 65536) { hdr[h++] = 126; hdr[h++] = (uint8_t)(len >> 8);
                            hdr[h++] = (uint8_t)(len & 0xFF); }
    else                  { hdr[h++] = 127;
                            for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)(len >> (i*8)); }
    pipe_write(&g_to_client, hdr, h);
    if (len) pipe_write(&g_to_client, payload, len);
}

static bool server_handshake(void)
{
    if (pipe_avail(&g_to_server) == 0) return false;

    char req[1024] = {0};
    size_t n = pipe_read(&g_to_server, req, sizeof(req) - 1);
    req[n] = '\0';
    if (strncmp(req, "GET ", 4) != 0) return false;

    const char *k = strstr(req, "Sec-WebSocket-Key:");
    if (!k) return false;
    k += strlen("Sec-WebSocket-Key:");
    while (*k == ' ') k++;
    char key[64] = {0};
    for (size_t i = 0; *k && *k != '\r' && i + 1 < sizeof(key); i++) key[i] = *k++;

    char accept[64];
    ws_compute_accept(key, accept, sizeof(accept));

    char resp[256];
    int rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "Sec-WebSocket-Protocol: ocpp1.6\r\n\r\n", accept);
    pipe_write(&g_to_client, resp, (size_t)rn);
    g_handshaken = true;
    return true;
}

/** Consume any complete client frames, unmasking and recording them. */
static void server_read_frames(void)
{
    for (;;) {
        size_t avail = pipe_avail(&g_to_server);
        if (avail < 2) return;

        const uint8_t *f = &g_to_server.buf[g_to_server.tail];
        uint8_t  opcode = f[0] & 0x0F;
        bool     masked = (f[1] & 0x80) != 0;
        uint64_t len    = f[1] & 0x7F;
        size_t   hdr    = 2;

        if (len == 126)      { if (avail < 4)  return;
                               len = ((uint64_t)f[2]<<8)|f[3]; hdr = 4; }
        else if (len == 127) { if (avail < 10) return;
                               len = 0;
                               for (int i=0;i<8;i++) len = (len<<8)|f[2+i];
                               hdr = 10; }

        uint8_t mkey[4] = {0};
        if (masked) { if (avail < hdr + 4) return; memcpy(mkey, f+hdr, 4); hdr += 4; }
        else        { g_saw_unmasked = true; }

        if (avail < hdr + len) return;

        if (opcode == 0x1 || opcode == 0x0) {
            size_t take = (len < sizeof(g_server_rx) - 1) ? (size_t)len
                                                          : sizeof(g_server_rx) - 1;
            for (size_t i = 0; i < take; i++) {
                g_server_rx[i] = (char)(masked ? (f[hdr+i] ^ mkey[i & 3]) : f[hdr+i]);
            }
            g_server_rx[take] = '\0';
            g_server_rx_len = take;
        } else if (opcode == 0xA) {
            g_server_pongs++;
        } else if (opcode == 0x8) {
            g_server_closes++;
        }

        g_to_server.tail += hdr + (size_t)len;
    }
}

/** When set, the fake server does nothing, leaving the pipe for inspection. */
static bool g_server_muted;

static void server_pump(void)
{
    if (g_server_muted) return;
    if (!g_handshaken) { server_handshake(); return; }
    server_read_frames();
}

static void server_reset(void)
{
    pipe_reset(&g_to_server);
    pipe_reset(&g_to_client);
    g_sock_open = true;
    g_handshaken = false;
    g_server_rx_len = 0;
    g_server_rx[0] = '\0';
    g_server_pongs = 0;
    g_server_closes = 0;
    g_saw_unmasked = false;
    g_server_muted = false;
}

/* ====================================================================== */
/* Tests                                                                  */
/* ====================================================================== */

static void test_accept_vector(void)
{
    printf("ws_compute_accept (RFC 6455 section 1.3 vector)\n");
    char out[32];
    ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out));
    CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "got '%s'", out);
}

static void test_base64(void)
{
    printf("ws_base64_encode (RFC 4648 vectors)\n");
    char o[32];
    ws_base64_encode((const uint8_t *)"",       0, o, sizeof(o)); CHECK(!strcmp(o, ""), "empty");
    ws_base64_encode((const uint8_t *)"f",      1, o, sizeof(o)); CHECK(!strcmp(o, "Zg=="), "f -> %s", o);
    ws_base64_encode((const uint8_t *)"fo",     2, o, sizeof(o)); CHECK(!strcmp(o, "Zm8="), "fo -> %s", o);
    ws_base64_encode((const uint8_t *)"foo",    3, o, sizeof(o)); CHECK(!strcmp(o, "Zm9v"), "foo -> %s", o);
    ws_base64_encode((const uint8_t *)"foob",   4, o, sizeof(o)); CHECK(!strcmp(o, "Zm9vYg=="), "foob -> %s", o);
    ws_base64_encode((const uint8_t *)"fooba",  5, o, sizeof(o)); CHECK(!strcmp(o, "Zm9vYmE="), "fooba -> %s", o);
    ws_base64_encode((const uint8_t *)"foobar", 6, o, sizeof(o)); CHECK(!strcmp(o, "Zm9vYmFy"), "foobar -> %s", o);
}

static void test_url_parse(void)
{
    printf("ws_parse_url\n");
    char host[64], path[64];
    uint16_t port; bool tls;

    CHECK(ws_parse_url("ws://example.com/ocpp", host, sizeof(host),
                       &port, path, sizeof(path), &tls), "plain ws");
    CHECK(!strcmp(host, "example.com") && port == 80 && !strcmp(path, "/ocpp") && !tls,
          "defaults: %s:%u%s tls=%d", host, port, path, tls);

    CHECK(ws_parse_url("wss://csms.example.com:9443/ocpp/CP001",
                       host, sizeof(host), &port, path, sizeof(path), &tls), "wss");
    CHECK(!strcmp(host, "csms.example.com") && port == 9443 &&
          !strcmp(path, "/ocpp/CP001") && tls, "explicit port + tls");

    CHECK(ws_parse_url("ws://10.0.0.5:9220", host, sizeof(host),
                       &port, path, sizeof(path), &tls), "no path");
    CHECK(!strcmp(path, "/") && port == 9220, "path defaults to /");

    CHECK(!ws_parse_url("http://example.com", host, sizeof(host),
                        &port, path, sizeof(path), &tls), "http rejected");
    CHECK(!ws_parse_url("example.com", host, sizeof(host),
                        &port, path, sizeof(path), &tls), "bare host rejected");
    CHECK(!ws_parse_url("ws://", host, sizeof(host),
                        &port, path, sizeof(path), &tls), "empty host rejected");
    CHECK(!ws_parse_url("ws://host:99999/x", host, sizeof(host),
                        &port, path, sizeof(path), &tls), "out-of-range port rejected");
}

static uint8_t g_rx[4096], g_tx[2048];

static ws_client_t *do_connect(void)
{
    server_reset();
    ws_client_t *ws = ws_create(g_rx, sizeof(g_rx), g_tx, sizeof(g_tx));
    bool ok = ws_connect(ws, "ws://csms.local:9220/ocpp/EVREST-0001",
                         "ocpp1.6", "EVREST-0001", "secret");
    CHECK(ok, "handshake completed");
    CHECK(ws_get_state(ws) == WS_OPEN, "state is OPEN");
    return ws;
}

static void test_handshake(void)
{
    printf("opening handshake\n");

    /* Mute the server so the request stays in the pipe for inspection; the
     * connect attempt is expected to fail, which is not what is under test. */
    server_reset();
    g_server_muted = true;
    ws_client_t *ws = ws_create(g_rx, sizeof(g_rx), g_tx, sizeof(g_tx));
    ws_connect(ws, "ws://csms.local:9220/ocpp/EVREST-0001",
               "ocpp1.6", "EVREST-0001", "secret");

    char req[1024] = {0};
    size_t n = pipe_read(&g_to_server, req, sizeof(req) - 1);
    req[n] = '\0';
    CHECK(n > 0, "client wrote a request");
    CHECK(strstr(req, "GET /ocpp/EVREST-0001 HTTP/1.1") != NULL,
          "request line: %.40s", req);
    CHECK(strstr(req, "Host: csms.local:9220") != NULL, "Host header");
    CHECK(strstr(req, "Upgrade: websocket") != NULL, "Upgrade header");
    CHECK(strstr(req, "Sec-WebSocket-Protocol: ocpp1.6") != NULL, "subprotocol");
    CHECK(strstr(req, "Sec-WebSocket-Version: 13") != NULL, "version 13");
    /* base64("EVREST-0001:secret") */
    CHECK(strstr(req, "Authorization: Basic RVZSRVNULTAwMDE6c2VjcmV0") != NULL,
          "basic auth header");

    /* With no credentials there must be no Authorization header at all,
     * rather than an empty one. */
    server_reset();
    g_server_muted = true;
    ws_client_t *ws2 = ws_create(g_rx, sizeof(g_rx), g_tx, sizeof(g_tx));
    ws_connect(ws2, "ws://csms.local:9220/ocpp/CP", "ocpp1.6", NULL, NULL);
    memset(req, 0, sizeof(req));
    pipe_read(&g_to_server, req, sizeof(req) - 1);
    CHECK(strstr(req, "Authorization:") == NULL, "no auth header when unset");
}

static void test_bad_accept_rejected(void)
{
    printf("handshake with a wrong accept value is rejected\n");
    server_reset();
    g_server_muted = true;   /* we answer by hand below */

    ws_client_t *ws = ws_create(g_rx, sizeof(g_rx), g_tx, sizeof(g_tx));

    /* Queue a 101 whose accept value is wrong. A server that gets this wrong
     * is not speaking WebSocket, and framing arbitrary bytes would be worse
     * than failing. */
    const char *bad =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
    pipe_write(&g_to_client, bad, strlen(bad));

    CHECK(!ws_connect(ws, "ws://x/y", "ocpp1.6", NULL, NULL), "rejected");
    CHECK(ws_get_state(ws) == WS_CLOSED, "socket closed after rejection");
}

static void test_send_is_masked(void)
{
    printf("client frames are masked\n");
    ws_client_t *ws = do_connect();

    const char *msg = "[2,\"1\",\"Heartbeat\",{}]";
    CHECK(ws_send_text(ws, msg, strlen(msg)), "send accepted");
    server_read_frames();

    CHECK(!g_saw_unmasked, "no unmasked client frame (RFC 6455 5.3)");
    CHECK(strcmp(g_server_rx, msg) == 0, "server got '%s'", g_server_rx);

    /* Two identical messages must not produce identical masks, or the mask is
     * not doing its job. */
    size_t before = pipe_avail(&g_to_server);
    ws_send_text(ws, msg, strlen(msg));
    const uint8_t *f1 = &g_to_server.buf[g_to_server.tail];
    uint8_t mask1[4];
    memcpy(mask1, f1 + 2, 4);
    (void)before;
    server_read_frames();
    ws_send_text(ws, msg, strlen(msg));
    const uint8_t *f2 = &g_to_server.buf[g_to_server.tail];
    CHECK(memcmp(mask1, f2 + 2, 4) != 0, "mask key varies between frames");
    server_read_frames();
}

static void test_receive_text(void)
{
    printf("receiving text frames\n");
    ws_client_t *ws = do_connect();

    const char *reply = "[3,\"1\",{\"currentTime\":\"2026-08-18T14:03:00Z\"}]";
    server_send_frame(0x1, reply, strlen(reply), true);

    const char *msg = NULL; size_t len = 0;
    ws_event_t e = ws_poll(ws, 10, &msg, &len);
    CHECK(e == WS_EVT_TEXT, "got a text event (%d)", e);
    CHECK(msg && strcmp(msg, reply) == 0, "payload '%s'", msg ? msg : "(null)");
    CHECK(len == strlen(reply), "length %u", (unsigned)len);
}

static void test_fragmented_message(void)
{
    printf("fragmented messages are reassembled\n");
    ws_client_t *ws = do_connect();

    /* A 3-fragment text message: TEXT(not fin) + CONT(not fin) + CONT(fin). */
    server_send_frame(0x1, "[3,\"1\",", 7, false);
    server_send_frame(0x0, "{\"status\":", 10, false);
    server_send_frame(0x0, "\"Accepted\"}]", 12, true);

    const char *msg = NULL; size_t len = 0;
    ws_event_t e = WS_EVT_NONE;
    for (int i = 0; i < 8 && e != WS_EVT_TEXT; i++) {
        e = ws_poll(ws, 10, &msg, &len);
    }
    CHECK(e == WS_EVT_TEXT, "reassembled event");
    CHECK(msg && strcmp(msg, "[3,\"1\",{\"status\":\"Accepted\"}]") == 0,
          "reassembled payload '%s'", msg ? msg : "(null)");
}

static void test_ping_pong(void)
{
    printf("ping is answered with a matching pong\n");
    ws_client_t *ws = do_connect();

    server_send_frame(0x9, "hb", 2, true);      /* server ping with a payload */
    const char *msg = NULL; size_t len = 0;
    ws_poll(ws, 10, &msg, &len);
    server_read_frames();
    CHECK(g_server_pongs == 1, "client sent exactly one pong, got %d", g_server_pongs);
}

static void test_large_and_oversize(void)
{
    printf("extended length and oversize handling\n");
    ws_client_t *ws = do_connect();

    /* 300 bytes exercises the 16-bit extended length path. */
    char big[301];
    memset(big, 'x', 300);
    big[300] = '\0';
    server_send_frame(0x1, big, 300, true);

    const char *msg = NULL; size_t len = 0;
    ws_event_t e = ws_poll(ws, 10, &msg, &len);
    CHECK(e == WS_EVT_TEXT && len == 300, "300-byte frame, got event %d len %u",
          e, (unsigned)len);

    /* A message larger than the assembly buffer must close the connection
     * rather than deliver truncated, invalid JSON. */
    ws = do_connect();
    static char huge[6000];
    memset(huge, 'y', sizeof(huge));
    server_send_frame(0x1, huge, sizeof(huge), true);
    e = WS_EVT_NONE;
    for (int i = 0; i < 16 && e == WS_EVT_NONE; i++) {
        e = ws_poll(ws, 10, &msg, &len);
    }
    CHECK(e == WS_EVT_CLOSED, "oversize message closes the connection (%d)", e);
}

static void test_close(void)
{
    printf("close handshake\n");
    ws_client_t *ws = do_connect();

    ws_close(ws, 1000);
    server_read_frames();
    CHECK(g_server_closes == 1, "client sent a close frame");
    CHECK(ws_get_state(ws) == WS_CLOSED, "state is CLOSED");

    /* Sending after close must fail rather than write to a dead socket. */
    CHECK(!ws_send_text(ws, "x", 1), "send after close is refused");
}

int main(void)
{
    printf("=== WebSocket tests ===\n");
    test_accept_vector();
    test_base64();
    test_url_parse();
    test_handshake();
    test_bad_accept_rejected();
    test_send_is_masked();
    test_receive_text();
    test_fragmented_message();
    test_ping_pong();
    test_large_and_oversize();
    test_close();
    if (g_fail == 0) { printf("\nAll tests passed.\n"); return 0; }
    printf("\n%d test(s) FAILED.\n", g_fail);
    return 1;
}
