#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "packet_codec.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t src_mac[APP_MAC_LEN];
    int8_t rssi;
    uint16_t len;
    uint8_t data[APP_ESPNOW_FRAME_MAX];
} espnow_rx_item_t;

esp_err_t espnow_link_init(uint8_t channel, const char *ap_ssid, const char *ap_password);
esp_err_t espnow_link_add_peer(const uint8_t mac[APP_MAC_LEN]);
bool espnow_link_peer_exists(const uint8_t mac[APP_MAC_LEN]);
esp_err_t espnow_link_send(const uint8_t mac[APP_MAC_LEN], const void *data, size_t len);
esp_err_t espnow_link_send_broadcast(const void *data, size_t len);
bool espnow_link_receive(espnow_rx_item_t *item, TickType_t timeout);
void espnow_link_get_sta_mac(uint8_t mac[APP_MAC_LEN]);
void espnow_link_get_ap_mac(uint8_t mac[APP_MAC_LEN]);
uint8_t espnow_link_get_channel(void);
int espnow_link_get_ap_client_count(void);
const uint8_t *espnow_link_broadcast_mac(void);

#ifdef __cplusplus
}
#endif
