/**
 * @file  esp_at.c
 * @brief ESP-AT driver. See esp_at.h.
 */
#include "esp_at.h"
#include "evse_board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern UART_HandleTypeDef ESP32_UART_HANDLE;
#define ESP_UART (&ESP32_UART_HANDLE)

#define RX_RING_SIZE     2048u
#define AT_LINE_MAX      256u
#define AT_TIMEOUT_MS    5000u
#define JOIN_TIMEOUT_MS  20000u

/* ---------------------------------------------------------------------- */
/* Receive ring                                                           */
/* ---------------------------------------------------------------------- */

static volatile uint8_t  s_ring[RX_RING_SIZE];
static volatile uint32_t s_head, s_tail;
static uint8_t           s_rx_byte;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_mem;

/** Bytes waiting on link N, as reported by the most recent +IPD. */
static uint32_t s_pending[ESP_AT_MAX_LINKS];
static bool     s_link_open[ESP_AT_MAX_LINKS];

static bool     s_joined;
static char     s_ip[16];
static int8_t   s_rssi;
static char     s_ssid[CFG_STR_SSID_LEN];
static char     s_psk[CFG_STR_PSK_LEN];
static uint32_t s_next_join_ms;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

void esp_at_rx_isr(uint8_t byte)
{
    uint32_t next = (s_head + 1u) % RX_RING_SIZE;
    if (next != s_tail) {          /* drop on overflow rather than corrupt */
        s_ring[s_head] = byte;
        s_head = next;
    }
}

/** Re-arm the single-byte interrupt receive. Called from the HAL callback. */
static void rx_rearm(void)
{
    HAL_UART_Receive_IT(ESP_UART, &s_rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP32_UART_HANDLE.Instance) {
        esp_at_rx_isr(s_rx_byte);
        rx_rearm();
    }
}

static bool ring_getc(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        if (s_tail != s_head) {
            *out = s_ring[s_tail];
            s_tail = (s_tail + 1u) % RX_RING_SIZE;
            return true;
        }
        if ((int32_t)(now_ms() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void ring_flush(void) { s_tail = s_head; }

/**
 * Read one CRLF-terminated line.
 * @return length, or -1 on timeout. Empty lines are skipped, since ESP-AT
 *         pads its responses with them.
 */
static int read_line(char *out, size_t out_len, uint32_t timeout_ms)
{
    size_t n = 0;
    uint32_t deadline = now_ms() + timeout_ms;

    for (;;) {
        uint8_t c;
        int32_t left = (int32_t)(deadline - now_ms());
        if (left <= 0) return -1;
        if (!ring_getc(&c, (uint32_t)left)) return -1;

        if (c == '\n') {
            out[n] = '\0';
            if (n > 0 && out[n - 1] == '\r') out[--n] = '\0';
            if (n == 0) continue;              /* skip blank lines */
            return (int)n;
        }
        if (n + 1 < out_len) out[n++] = (char)c;
    }
}

/* ---------------------------------------------------------------------- */
/* Command layer                                                          */
/* ---------------------------------------------------------------------- */

static void at_write(const char *s)
{
    HAL_UART_Transmit(ESP_UART, (uint8_t *)s, (uint16_t)strlen(s), 1000);
}

/** Note a +IPD notification so esp_at_recv() knows there is data to pull. */
static bool handle_unsolicited(const char *line)
{
    if (strncmp(line, "+IPD,", 5) == 0) {
        int link = 0; unsigned len = 0;
        if (sscanf(line + 5, "%d,%u", &link, &len) == 2 &&
            link >= 0 && link < ESP_AT_MAX_LINKS) {
            s_pending[link] = len;
        }
        return true;
    }
    if (strstr(line, ",CLOSED") != NULL) {
        int link = atoi(line);
        if (link >= 0 && link < ESP_AT_MAX_LINKS) {
            s_link_open[link] = false;
            s_pending[link] = 0;
        }
        return true;
    }
    if (strcmp(line, "WIFI DISCONNECT") == 0) { s_joined = false; return true; }
    if (strcmp(line, "WIFI CONNECTED")  == 0) { return true; }
    if (strcmp(line, "WIFI GOT IP")     == 0) { s_joined = true; return true; }
    if (strcmp(line, "ready") == 0)           { return true; }
    return false;
}

/**
 * Send a command and wait for its terminal status.
 * @param prefix   optional response prefix to capture (e.g. "+CIPSTA:")
 * @param captured buffer for the captured line, may be NULL
 * @return true if the module answered OK.
 */
static bool at_command(const char *cmd, const char *prefix,
                       char *captured, size_t captured_len, uint32_t timeout_ms)
{
    char line[AT_LINE_MAX];

    ring_flush();
    at_write(cmd);
    at_write("\r\n");

    uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        int32_t left = (int32_t)(deadline - now_ms());
        if (left <= 0) return false;
        if (read_line(line, sizeof(line), (uint32_t)left) < 0) return false;

        if (strcmp(line, "OK") == 0)    return true;
        if (strcmp(line, "ERROR") == 0) return false;
        if (strcmp(line, "FAIL") == 0)  return false;

        if (prefix && captured && strncmp(line, prefix, strlen(prefix)) == 0) {
            snprintf(captured, captured_len, "%s", line + strlen(prefix));
        }
        (void)handle_unsolicited(line);
    }
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

bool esp_at_init(const char *ssid, const char *psk)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid ? ssid : "");
    snprintf(s_psk,  sizeof(s_psk),  "%s", psk  ? psk  : "");
    s_joined = false;
    memset(s_link_open, 0, sizeof(s_link_open));
    memset(s_pending, 0, sizeof(s_pending));

    rx_rearm();

    /* Hardware reset; ESP-AT prints "ready" when it has booted. */
    HAL_GPIO_WritePin(ESP32_RESET_GPIO_PORT, ESP32_RESET_GPIO_PIN, GPIO_PIN_RESET);
    vTaskDelay(pdMS_TO_TICKS(20));
    HAL_GPIO_WritePin(ESP32_RESET_GPIO_PORT, ESP32_RESET_GPIO_PIN, GPIO_PIN_SET);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ring_flush();

    if (!at_command("AT", NULL, NULL, 0, 2000))              return false;
    if (!at_command("ATE0", NULL, NULL, 0, 2000))            return false;
    if (!at_command("AT+CWMODE=1", NULL, NULL, 0, 2000))     return false;
    if (!at_command("AT+CIPMUX=1", NULL, NULL, 0, 2000))     return false;
    /* Passive receive — see the header for why this matters. */
    if (!at_command("AT+CIPRECVMODE=1", NULL, NULL, 0, 2000)) return false;
    /* Do not auto-reconnect: we drive reconnection so the backoff is ours. */
    (void)at_command("AT+CWAUTOCONN=0", NULL, NULL, 0, 2000);

    return true;
}

void esp_at_service(void)
{
    if (s_joined || s_ssid[0] == '\0') return;
    if ((int32_t)(now_ms() - s_next_join_ms) < 0) return;

    /*
     * Retry no faster than every 15 s. A charger that hammers AT+CWJAP at a
     * misconfigured AP keeps the UART permanently busy and starves the rest of
     * the driver.
     */
    s_next_join_ms = now_ms() + 15000u;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", s_ssid, s_psk);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = at_command(cmd, NULL, NULL, 0, JOIN_TIMEOUT_MS);
    if (ok) {
        char resp[64] = {0};
        if (at_command("AT+CIPSTA?", "+CIPSTA:ip:", resp, sizeof(resp), 2000)) {
            /* Response looks like "ip:"192.168.1.42"" — strip the quotes. */
            char *q1 = strchr(resp, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2) {
                size_t n = (size_t)(q2 - q1 - 1);
                if (n < sizeof(s_ip)) { memcpy(s_ip, q1 + 1, n); s_ip[n] = '\0'; }
            }
        }
        s_joined = (s_ip[0] != '\0');
    }
    xSemaphoreGive(s_lock);
}

bool esp_at_is_connected(void) { return s_joined; }

void esp_at_get_ip(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", s_ip);
}

int8_t esp_at_rssi(void)
{
    if (!s_joined) return 0;
    char resp[128] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* +CWJAP:<ssid>,<bssid>,<channel>,<rssi>,... */
    if (at_command("AT+CWJAP?", "+CWJAP:", resp, sizeof(resp), 2000)) {
        int commas = 0;
        for (const char *p = resp; *p; p++) {
            if (*p == ',' && ++commas == 3) { s_rssi = (int8_t)atoi(p + 1); break; }
        }
    }
    xSemaphoreGive(s_lock);
    return s_rssi;
}

int esp_at_connect(const char *host, uint16_t port, bool tls, uint32_t timeout_ms)
{
    if (!s_joined) return -1;

    int link = -1;
    for (int i = 0; i < ESP_AT_MAX_LINKS; i++) {
        if (!s_link_open[i]) { link = i; break; }
    }
    if (link < 0) return -1;

    char cmd[192];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=%d,\"%s\",\"%s\",%u",
             link, tls ? "SSL" : "TCP", host, port);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = at_command(cmd, NULL, NULL, 0, timeout_ms);
    if (ok) {
        s_link_open[link] = true;
        s_pending[link] = 0;
    }
    xSemaphoreGive(s_lock);

    return ok ? link : -1;
}

net_err_t esp_at_send(int link, const void *data, size_t len, uint32_t timeout_ms)
{
    if (link < 0 || link >= ESP_AT_MAX_LINKS || !s_link_open[link]) {
        return NET_ERR_CLOSED;
    }

    /*
     * ESP-AT caps one CIPSEND at 2048 bytes, so anything larger is chunked.
     * OCPP frames are usually well under this, but a GetConfiguration response
     * or a long local auth list will exceed it.
     */
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    net_err_t result = NET_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 2048u) chunk = 2048u;

        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u", link, (unsigned)chunk);
        ring_flush();
        at_write(cmd);
        at_write("\r\n");

        /* The module answers with '>' when it is ready for the payload. */
        bool prompt = false;
        uint32_t deadline = now_ms() + timeout_ms;
        while (!prompt) {
            uint8_t c;
            int32_t left = (int32_t)(deadline - now_ms());
            if (left <= 0) { result = NET_ERR_TIMEOUT; goto done; }
            if (!ring_getc(&c, (uint32_t)left)) { result = NET_ERR_TIMEOUT; goto done; }
            if (c == '>') prompt = true;
        }

        HAL_UART_Transmit(ESP_UART, (uint8_t *)(uintptr_t)(p + sent),
                          (uint16_t)chunk, timeout_ms);

        /* Wait for SEND OK before starting the next chunk, or the module will
         * interleave the two and drop bytes. */
        char line[AT_LINE_MAX];
        bool acked = false;
        while (!acked) {
            int32_t left = (int32_t)(deadline - now_ms());
            if (left <= 0) { result = NET_ERR_TIMEOUT; goto done; }
            if (read_line(line, sizeof(line), (uint32_t)left) < 0) {
                result = NET_ERR_TIMEOUT; goto done;
            }
            if (strcmp(line, "SEND OK") == 0)   { acked = true; }
            else if (strcmp(line, "SEND FAIL") == 0 || strcmp(line, "ERROR") == 0) {
                result = NET_ERR_CLOSED; goto done;
            } else {
                (void)handle_unsolicited(line);
            }
        }
        sent += chunk;
    }

done:
    xSemaphoreGive(s_lock);
    return result;
}

net_err_t esp_at_recv(int link, void *buf, size_t len, size_t *received,
                      uint32_t timeout_ms)
{
    *received = 0;
    if (link < 0 || link >= ESP_AT_MAX_LINKS) return NET_ERR_INTERNAL;
    if (!s_link_open[link])                   return NET_ERR_CLOSED;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Drain any notifications sitting in the ring so s_pending is current. */
    char line[AT_LINE_MAX];
    while (read_line(line, sizeof(line), 0) > 0) {
        (void)handle_unsolicited(line);
    }

    if (s_pending[link] == 0u) {
        xSemaphoreGive(s_lock);
        /* Nothing announced yet. Sleep outside the lock so a concurrent send
         * is not blocked for the whole timeout. */
        vTaskDelay(pdMS_TO_TICKS(timeout_ms > 20u ? 20u : timeout_ms));
        return NET_ERR_TIMEOUT;
    }

    size_t want = s_pending[link];
    if (want > len) want = len;

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CIPRECVDATA=%d,%u", link, (unsigned)want);
    ring_flush();
    at_write(cmd);
    at_write("\r\n");

    /*
     * Response is "+CIPRECVDATA:<actual_len>,<data>" followed by OK. The data
     * is binary and may contain CR, LF or a comma, so it is read by length
     * rather than by line — parsing it as text would corrupt any WebSocket
     * frame containing those bytes.
     */
    net_err_t result = NET_ERR_TIMEOUT;
    uint32_t deadline = now_ms() + timeout_ms;

    for (;;) {
        int32_t left = (int32_t)(deadline - now_ms());
        if (left <= 0) break;

        /* Read the header up to and including the comma after the length. */
        char hdr[48];
        size_t hn = 0;
        bool have_hdr = false;
        while (hn + 1 < sizeof(hdr)) {
            uint8_t c;
            left = (int32_t)(deadline - now_ms());
            if (left <= 0) goto out;
            if (!ring_getc(&c, (uint32_t)left)) goto out;
            hdr[hn++] = (char)c;
            hdr[hn] = '\0';
            if (c == ',' && strstr(hdr, "+CIPRECVDATA:") != NULL) { have_hdr = true; break; }
            if (c == '\n') { hn = 0; hdr[0] = '\0'; }   /* not our line; restart */
        }
        if (!have_hdr) goto out;

        const char *colon = strchr(hdr, ':');
        unsigned actual = colon ? (unsigned)atoi(colon + 1) : 0u;
        if (actual > len) actual = (unsigned)len;

        uint8_t *dst = (uint8_t *)buf;
        for (unsigned i = 0; i < actual; i++) {
            uint8_t c;
            left = (int32_t)(deadline - now_ms());
            if (left <= 0) goto out;
            if (!ring_getc(&c, (uint32_t)left)) goto out;
            dst[i] = c;
        }

        *received = actual;
        s_pending[link] = (s_pending[link] > actual) ? (s_pending[link] - actual) : 0u;
        result = (actual > 0u) ? NET_OK : NET_ERR_TIMEOUT;
        break;
    }

out:
    xSemaphoreGive(s_lock);
    return result;
}

void esp_at_close(int link)
{
    if (link < 0 || link >= ESP_AT_MAX_LINKS) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", link);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    (void)at_command(cmd, NULL, NULL, 0, 3000);
    s_link_open[link] = false;
    s_pending[link] = 0;
    xSemaphoreGive(s_lock);
}
