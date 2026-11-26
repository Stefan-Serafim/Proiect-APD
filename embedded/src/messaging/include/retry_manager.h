#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "message_queue.h"
#include "packet_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAT_MAX_PEERS      8u
#define TEST_MAX_SAMPLES    500u

typedef struct {
    bool used;
    bool online;
    uint8_t node_id;
    uint8_t mac[APP_MAC_LEN];
    char name[16];
    int8_t rssi;
    uint32_t last_seen_ms;
} chat_peer_t;

typedef struct {
    uint32_t seq_index;
    float rtt_ms;
    bool acked;
    uint8_t retries;
    int8_t rssi;
} test_sample_t;

typedef struct {
    bool running;
    uint32_t test_id;
    uint32_t total_count;
    uint32_t completed_count;
    uint32_t acked_first_try;
    uint32_t retried_ok;
    uint32_t lost;
    float avg_rtt_ms;
    float min_rtt_ms;
    float max_rtt_ms;
    float median_rtt_ms;
    float p95_rtt_ms;
    float stddev_rtt_ms;
    float avg_rssi;
    float duration_s;
    size_t sample_count;
} test_summary_t;

typedef struct {
    uint32_t tx_sent;
    uint32_t tx_acked_first;
    uint32_t tx_retried_ack;
    uint32_t tx_retransmissions;
    uint32_t tx_failed;
    uint32_t rx_received;
    uint32_t rx_duplicates;
    uint32_t sim_loss_pct;
} messaging_stats_t;

typedef void (*peer_event_cb_t)(const chat_peer_t *peer, void *ctx);
typedef void (*test_event_cb_t)(const test_summary_t *summary, bool finished, void *ctx);

esp_err_t retry_manager_init(uint8_t self_id, const char *self_name);
void retry_manager_register_peer_cb(peer_event_cb_t cb, void *ctx);
void retry_manager_register_test_cb(test_event_cb_t cb, void *ctx);

esp_err_t retry_manager_send_text(const char *text, uint32_t *out_msg_id);
esp_err_t retry_manager_send_text_to(const uint8_t dst_mac[APP_MAC_LEN], const char *text, uint32_t *out_msg_id);
void retry_manager_send_hello(void);
void retry_manager_handle_packet(const uint8_t src_mac[APP_MAC_LEN], int8_t rssi, const app_packet_view_t *packet);
void retry_manager_check_peer_timeouts(void);

esp_err_t retry_manager_start_test(unsigned count, bool reliable_mode);
void retry_manager_get_test_summary(test_summary_t *out);
size_t retry_manager_get_test_samples(test_sample_t *out, size_t max_samples);
size_t retry_manager_export_csv(char *buf, size_t buf_size);

size_t retry_manager_get_peers(chat_peer_t *out, size_t max_peers);
bool retry_manager_get_active_peer(chat_peer_t *out);
bool retry_manager_select_peer_by_id(uint8_t node_id);
void retry_manager_get_stats(messaging_stats_t *out);

void retry_manager_set_loss_percent(unsigned percent);
unsigned retry_manager_get_loss_percent(void);
void retry_manager_drop_next_ack(void);

#ifdef __cplusplus
}
#endif
