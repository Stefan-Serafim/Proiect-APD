#include "espnow_link.h"

#include <string.h>
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "espnow_link";
static const uint8_t s_broadcast[APP_MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static QueueHandle_t s_rx_queue = NULL;
static uint8_t s_sta_mac[APP_MAC_LEN];
static uint8_t s_ap_mac[APP_MAC_LEN];
static uint8_t s_channel = 6;
static TaskHandle_t s_dns_task = NULL;

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !info->src_addr || !data || len <= 0 || len > (int)APP_ESPNOW_FRAME_MAX || !s_rx_queue) {
        return;
    }
    espnow_rx_item_t item = {};
    memcpy(item.src_mac, info->src_addr, APP_MAC_LEN);
    item.len = (uint16_t)len;
    memcpy(item.data, data, (size_t)len);
    if (info->rx_ctrl) {
        item.rssi = (int8_t)info->rx_ctrl->rssi;
    }
    (void)xQueueSend(s_rx_queue, &item, 0);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS && tx_info) {
        ESP_LOGD(TAG, "MAC-layer send failure");
    }
}
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "MAC-layer send failure");
    }
}
#endif

/*
 * Lightweight Captive Portal DNS server (UDP/53).
 * Resolves A-record queries to 192.168.4.1 so mobile devices keep the
 * SoftAP association alive and pop up the local chat interface offline.
 */
static void captive_dns_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Captive DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "Captive DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    const uint32_t ap_ip = inet_addr("192.168.4.1");

    for (;;) {
        struct sockaddr_in client = {};
        socklen_t client_len = sizeof(client);
        const int rx_len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &client_len);
        if (rx_len < 12 || rx_len > (int)(sizeof(buf) - 16)) {
            continue;
        }

        const uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qdcount == 0) {
            continue;
        }

        // Walk QNAME safely to find end of question section
        int pos = 12;
        while (pos < rx_len && buf[pos] != 0) {
            const uint8_t label_len = buf[pos];
            if ((label_len & 0xC0u) != 0) {
                pos += 2;
                break;
            }
            pos += 1 + label_len;
        }
        if (pos < rx_len && buf[pos] == 0) {
            pos += 1;
        }
        if (pos + 4 > rx_len) {
            continue;
        }

        const uint16_t qtype = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        const int q_end = pos + 4;

        // Build DNS response in-place
        buf[2] = 0x81; // QR=1, RD=1
        buf[3] = 0x80; // RA=1, RCODE=0
        buf[6] = 0x00;
        buf[7] = (qtype == 1) ? 0x01 : 0x00; // Answer count: 1 for A-record, 0 otherwise
        buf[8] = 0x00;
        buf[9] = 0x00;
        buf[10] = 0x00;
        buf[11] = 0x00;

        int tx_len = q_end;
        if (qtype == 1 && q_end + 16 <= (int)sizeof(buf)) {
            uint8_t *ans = buf + q_end;
            ans[0] = 0xC0; ans[1] = 0x0C; // Pointer to QNAME at offset 12
            ans[2] = 0x00; ans[3] = 0x01; // TYPE A
            ans[4] = 0x00; ans[5] = 0x01; // CLASS IN
            ans[6] = 0x00; ans[7] = 0x00; ans[8] = 0x00; ans[9] = 60; // TTL 60s
            ans[10] = 0x00; ans[11] = 0x04; // RDLENGTH 4
            memcpy(ans + 12, &ap_ip, 4);
            tx_len += 16;
        }

        (void)sendto(sock, buf, (size_t)tx_len, 0, (struct sockaddr *)&client, client_len);
    }
}

esp_err_t espnow_link_add_peer(const uint8_t mac[APP_MAC_LEN])
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, APP_MAC_LEN);
    peer.channel = s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    const esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        return ESP_OK;
    }
    return err;
}

bool espnow_link_peer_exists(const uint8_t mac[APP_MAC_LEN])
{
    return mac && esp_now_is_peer_exist(mac);
}

esp_err_t espnow_link_init(uint8_t channel, const char *ap_ssid, const char *ap_password)
{
    s_channel = channel;
    s_rx_queue = xQueueCreate(32, sizeof(espnow_rx_item_t));
    if (!s_rx_queue) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_ap();
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {};
    const char *ssid = (ap_ssid && ap_ssid[0]) ? ap_ssid : "APD-Node";
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = channel;
    ap_cfg.ap.max_connection = 4;

    if (ap_password && strlen(ap_password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, ap_password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Disable Wi-Fi power save so ESP-NOW RTT stays consistently low (< 50ms)
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, s_ap_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(espnow_link_add_peer(s_broadcast));

    if (!s_dns_task) {
        xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 3, &s_dns_task);
    }

    ESP_LOGI(TAG, "SoftAP '%s' (192.168.4.1) + ESP-NOW ready on ch %u, STA MAC %02X:%02X:%02X:%02X:%02X:%02X",
             (const char *)ap_cfg.ap.ssid, channel,
             s_sta_mac[0], s_sta_mac[1], s_sta_mac[2], s_sta_mac[3], s_sta_mac[4], s_sta_mac[5]);
    return ESP_OK;
}

esp_err_t espnow_link_send(const uint8_t mac[APP_MAC_LEN], const void *data, size_t len)
{
    if (!mac || !data || len == 0 || len > APP_ESPNOW_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = espnow_link_add_peer(mac);
    if (err != ESP_OK) {
        return err;
    }
    return esp_now_send(mac, (const uint8_t *)data, len);
}

esp_err_t espnow_link_send_broadcast(const void *data, size_t len)
{
    return espnow_link_send(s_broadcast, data, len);
}

bool espnow_link_receive(espnow_rx_item_t *item, TickType_t timeout)
{
    return item && s_rx_queue && xQueueReceive(s_rx_queue, item, timeout) == pdTRUE;
}

void espnow_link_get_sta_mac(uint8_t mac[APP_MAC_LEN])
{
    if (mac) {
        memcpy(mac, s_sta_mac, APP_MAC_LEN);
    }
}

void espnow_link_get_ap_mac(uint8_t mac[APP_MAC_LEN])
{
    if (mac) {
        memcpy(mac, s_ap_mac, APP_MAC_LEN);
    }
}

uint8_t espnow_link_get_channel(void)
{
    return s_channel;
}

int espnow_link_get_ap_client_count(void)
{
    wifi_sta_list_t sta_list = {};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
}

const uint8_t *espnow_link_broadcast_mac(void)
{
    return s_broadcast;
}
