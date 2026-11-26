#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "app_core.h"
#include "esp_log.h"
#include "esp_system.h"
#include "espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "message_queue.h"
#include "nvs_flash.h"
#include "packet_codec.h"
#include "retry_manager.h"
#include "web_server.h"
#include "ws_handler.h"

static const char *TAG = "main";

static void format_mac(char *out, size_t n, const uint8_t mac[APP_MAC_LEN])
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void print_peers(void)
{
    chat_peer_t peers[CHAT_MAX_PEERS];
    const size_t n = retry_manager_get_peers(peers, CHAT_MAX_PEERS);
    printf("\nKnown peers (%u):\n", (unsigned)n);
    for (size_t i = 0; i < n; ++i) {
        char mac[24];
        format_mac(mac, sizeof(mac), peers[i].mac);
        printf("  id=%u  %-14s  %-7s  RSSI=%d  MAC=%s\n",
               peers[i].node_id,
               peers[i].name,
               peers[i].online ? "ONLINE" : "OFFLINE",
               peers[i].rssi,
               mac);
    }
    if (!n) {
        printf("  none yet; waiting for HELLO beacons...\n");
    }
    printf("\n");
}

static void print_help(void)
{
    printf("\nSerial Debug Commands:\n");
    printf("  <text>           send chat message via EMB-2 queue\n");
    printf("  /peers           list discovered peers\n");
    printf("  /to <id>         select destination peer ID\n");
    printf("  /test [count]    run PING/PONG burst test\n");
    printf("  /loss <0..100>   simulate incoming packet loss\n");
    printf("  /dropnextack     skip one DATA ACK once (demo retry + dedup)\n");
    printf("  /info            show node, SoftAP, and queue stats\n");
    printf("  /help            show this help\n\n");
}

static void rx_task(void *arg)
{
    (void)arg;
    espnow_rx_item_t rx;
    for (;;) {
        if (!espnow_link_receive(&rx, portMAX_DELAY)) {
            continue;
        }
        app_packet_view_t packet;
        const esp_err_t err = app_packet_decode(rx.data, rx.len, &packet);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Dropped invalid frame (%s)", esp_err_to_name(err));
            continue;
        }
        retry_manager_handle_packet(rx.src_mac, rx.rssi, &packet);
    }
}

static void beacon_task(void *arg)
{
    (void)arg;
    for (;;) {
        retry_manager_send_hello();
        retry_manager_check_peer_timeouts();
        ws_handler_broadcast_status();
        vTaskDelay(pdMS_TO_TICKS(APP_BEACON_PERIOD_MS));
    }
}

static void handle_command(char *line)
{
    while (*line && isspace((unsigned char)*line)) ++line;
    size_t len = strlen(line);
    while (len && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
    if (!len) return;

    if (line[0] != '/') {
        const esp_err_t err = retry_manager_send_text(line, NULL);
        if (err != ESP_OK) {
            printf("Queue failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (!strcmp(line, "/help")) {
        print_help();
    } else if (!strcmp(line, "/peers")) {
        print_peers();
    } else if (!strncmp(line, "/to ", 4)) {
        const int id = atoi(line + 4);
        if (id > 0 && id < 255 && retry_manager_select_peer_by_id((uint8_t)id)) {
            printf("Selected peer id=%d.\n", id);
        } else {
            printf("Peer id %d not found.\n", id);
        }
    } else if (!strncmp(line, "/test", 5)) {
        unsigned count = 100;
        if (strlen(line) > 5) count = (unsigned)strtoul(line + 5, NULL, 10);
        if (count == 0) count = 100;
        const esp_err_t err = retry_manager_start_test(count, true);
        printf("%s\n", err == ESP_OK ? "Burst test started." : esp_err_to_name(err));
    } else if (!strncmp(line, "/loss ", 6)) {
        unsigned pct = (unsigned)strtoul(line + 6, NULL, 10);
        if (pct > 100) pct = 100;
        retry_manager_set_loss_percent(pct);
        ws_handler_broadcast_status();
        printf("Simulated incoming loss set to %u%%.\n", pct);
    } else if (!strcmp(line, "/dropnextack")) {
        retry_manager_drop_next_ack();
        printf("Next incoming DATA ACK will be dropped once.\n");
    } else if (!strcmp(line, "/info")) {
        uint8_t mac[APP_MAC_LEN];
        char macs[24];
        espnow_link_get_sta_mac(mac);
        format_mac(macs, sizeof(macs), mac);
        messaging_stats_t st = {};
        retry_manager_get_stats(&st);
        printf("Node: %s (id=%d) | AP: %s (192.168.4.1, clients=%d) | ch=%d | MAC=%s\n",
               CONFIG_APP_NODE_NAME, CONFIG_APP_NODE_ID,
               CONFIG_APP_AP_SSID, espnow_link_get_ap_client_count(),
               CONFIG_ESPNOW_CHANNEL, macs);
        printf("EMB-2 Stats: sent=%" PRIu32 " ack1=%" PRIu32 " retried=%" PRIu32
               " retrans=%" PRIu32 " fail=%" PRIu32 " rx=%" PRIu32 " dup=%" PRIu32 "\n",
               st.tx_sent, st.tx_acked_first, st.tx_retried_ack,
               st.tx_retransmissions, st.tx_failed, st.rx_received, st.rx_duplicates);
    } else {
        printf("Unknown command. Type /help.\n");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[256];
    size_t pos = 0;

    print_help();
    printf("%s > ", CONFIG_APP_NODE_NAME);
    fflush(stdout);

    for (;;) {
        const int ch = fgetc(stdin);
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (pos == 0) continue;
            line[pos] = '\0';
            printf("\r\n");
            handle_command(line);
            pos = 0;
            printf("%s > ", CONFIG_APP_NODE_NAME);
            fflush(stdout);
            continue;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (pos > 0) {
                --pos;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (pos < sizeof(line) - 1 && (uint8_t)ch >= 0x20) {
            line[pos++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n==================================================\n");
    printf("%s | %s (id=%d) | AP: %s\n",
           APP_PROJECT_TITLE, CONFIG_APP_NODE_NAME, CONFIG_APP_NODE_ID, CONFIG_APP_AP_SSID);
    printf("==================================================\n");

    ESP_ERROR_CHECK(espnow_link_init(CONFIG_ESPNOW_CHANNEL, CONFIG_APP_AP_SSID, CONFIG_APP_AP_PASSWORD));
    ESP_ERROR_CHECK(message_queue_init(true));
    ESP_ERROR_CHECK(retry_manager_init(CONFIG_APP_NODE_ID, CONFIG_APP_NODE_NAME));

    message_queue_register_callback(ws_handler_broadcast_msg_event, NULL);
    retry_manager_register_peer_cb(ws_handler_broadcast_peer_event, NULL);
    retry_manager_register_test_cb(ws_handler_broadcast_test_event, NULL);

    ESP_ERROR_CHECK(web_server_start());

    xTaskCreate(rx_task, "rx_task", 6144, NULL, 8, NULL);
    xTaskCreate(beacon_task, "beacon_task", 4096, NULL, 4, NULL);
    xTaskCreate(console_task, "console_task", 5120, NULL, 3, NULL);
}
