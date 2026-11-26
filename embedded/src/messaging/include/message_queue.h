#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "packet_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSG_QUEUE_CAPACITY    32u
#define MSG_HISTORY_CAPACITY  80u

typedef enum {
    MSG_STATUS_QUEUED    = 0,
    MSG_STATUS_SENDING   = 1,
    MSG_STATUS_DELIVERED = 2,
    MSG_STATUS_FAILED    = 3,
    MSG_STATUS_RECEIVED  = 4,
} msg_status_t;

typedef struct {
    uint32_t msg_id;
    uint16_t sequence;
    uint8_t sender_id;
    uint8_t peer_id;
    bool is_outgoing;
    msg_status_t status;
    uint8_t retries_used;
    int8_t rssi;
    uint32_t timestamp_ms;
    uint32_t rtt_ms;
    char sender_name[16];
    char text[APP_MAX_PAYLOAD + 1];
} chat_message_t;

typedef struct {
    bool used;
    uint32_t msg_id;
    uint16_t sequence;
    uint8_t dst_mac[APP_MAC_LEN];
    bool has_dst_mac;
    uint8_t attempts;
    uint8_t max_retries;
    uint32_t ack_timeout_ms;
    int64_t first_tx_us;
    int64_t last_tx_us;
    bool waiting_ack;
    uint16_t text_len;
    char text[APP_MAX_PAYLOAD + 1];
} tx_queue_item_t;

typedef enum {
    MSG_EVENT_ADDED = 1,
    MSG_EVENT_STATUS_CHANGED = 2,
    MSG_EVENT_HISTORY_CLEARED = 3,
} msg_event_type_t;

typedef void (*msg_event_cb_t)(msg_event_type_t event, const chat_message_t *msg, void *ctx);

esp_err_t message_queue_init(bool enable_storage);
void message_queue_register_callback(msg_event_cb_t cb, void *ctx);

esp_err_t message_queue_enqueue_outgoing(uint32_t msg_id,
                                         uint16_t sequence,
                                         uint8_t self_id,
                                         const char *self_name,
                                         uint8_t peer_id,
                                         const uint8_t dst_mac[APP_MAC_LEN],
                                         bool has_dst_mac,
                                         const char *text,
                                         uint8_t max_retries,
                                         uint32_t ack_timeout_ms,
                                         uint32_t timestamp_ms);

esp_err_t message_queue_add_incoming(uint32_t msg_id,
                                     uint16_t sequence,
                                     uint8_t sender_id,
                                     const char *sender_name,
                                     uint8_t self_id,
                                     int8_t rssi,
                                     uint32_t timestamp_ms,
                                     const uint8_t *payload,
                                     size_t payload_len);

bool message_queue_get_next_tx(tx_queue_item_t *out);
void message_queue_mark_tx_attempt(uint32_t msg_id, int64_t now_us, const uint8_t dst_mac[APP_MAC_LEN], bool has_dst_mac);
bool message_queue_complete_ack(uint32_t msg_id, int64_t now_us);
bool message_queue_mark_failed(uint32_t msg_id);

size_t message_queue_get_history(chat_message_t *out, size_t max_items);
void message_queue_clear_history(void);
size_t message_queue_pending_count(void);
const char *msg_status_name(msg_status_t status);

#ifdef __cplusplus
}
#endif
