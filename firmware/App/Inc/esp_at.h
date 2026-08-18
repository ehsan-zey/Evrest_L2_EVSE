/**
 * @file  esp_at.h
 * @brief ESP32 Wi-Fi co-processor driver (ESP-AT firmware) on USART1.
 *
 * The ESP32 runs stock ESP-AT and terminates TLS itself, so the MCU never
 * carries a second TLS stack or certificate store for the Wi-Fi path.
 *
 * Receive uses **passive mode** (`AT+CIPRECVMODE=1`): the ESP32 sends only a
 * short `+IPD,<link>,<len>` notification and holds the payload until we ask
 * for it with `AT+CIPRECVDATA`. In active mode the payload arrives unsolicited
 * and can land in the middle of a command's response, which turns every AT
 * exchange into a parsing race. Passive mode makes the UART strictly
 * request/response.
 */
#ifndef ESP_AT_H
#define ESP_AT_H

#include "net_link.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** Concurrent connections. ESP-AT supports 5; the charger needs one. */
#define ESP_AT_MAX_LINKS   2

/**
 * Reset the ESP32, put it into station mode and start joining @p ssid.
 * Returns once the module answers, not once it has associated — association is
 * driven by esp_at_service().
 */
bool esp_at_init(const char *ssid, const char *psk);

/** Drive association and recovery. Call about once a second when not joined. */
void esp_at_service(void);

bool esp_at_is_connected(void);
void esp_at_get_ip(char *out, size_t out_len);
int8_t esp_at_rssi(void);

/** @return link id 0..ESP_AT_MAX_LINKS-1, or -1. */
int esp_at_connect(const char *host, uint16_t port, bool tls, uint32_t timeout_ms);

net_err_t esp_at_send(int link, const void *data, size_t len, uint32_t timeout_ms);
net_err_t esp_at_recv(int link, void *buf, size_t len, size_t *received,
                      uint32_t timeout_ms);
void esp_at_close(int link);

/** UART receive-complete hook; feeds the ring buffer. */
void esp_at_rx_isr(uint8_t byte);

#endif /* ESP_AT_H */
