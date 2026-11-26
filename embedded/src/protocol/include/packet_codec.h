#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PROTO_MAGIC       0xA55Au
#define APP_PROTO_VERSION     1u
#define APP_ESPNOW_FRAME_MAX  250u
#define APP_MAC_LEN           6u

typedef enum {
    MSG_TYPE_DATA  = 1,
    MSG_TYPE_ACK   = 2,
    MSG_TYPE_PING  = 3,
    MSG_TYPE_PONG  = 4,
    MSG_TYPE_HELLO = 5,
} app_msg_type_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t sender_id;
    uint8_t flags;
    uint32_t msg_id;
    uint16_t sequence;
    uint16_t payload_length;
    uint32_t sender_ms;
    uint32_t packet_crc32;
} app_packet_header_t;

#define APP_MAX_PAYLOAD (APP_ESPNOW_FRAME_MAX - sizeof(app_packet_header_t))

typedef struct {
    app_packet_header_t header;
    const uint8_t *payload;
} app_packet_view_t;

typedef struct __attribute__((packed)) {
    uint32_t ack_msg_id;
    uint16_t ack_sequence;
    uint8_t acked_type;
    uint8_t status;
} app_ack_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t test_id;
    uint32_t seq_index;
    uint32_t token;
    uint32_t tx_ms;
} app_ping_payload_t;

typedef struct __attribute__((packed)) {
    char name[16];
    uint8_t channel;
    uint8_t flags;
    uint16_t reserved;
} app_hello_payload_t;

uint32_t app_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t app_crc32(const uint8_t *data, size_t len);

esp_err_t app_packet_encode(uint8_t type,
                            uint32_t msg_id,
                            uint16_t sequence,
                            uint8_t sender_id,
                            uint8_t flags,
                            uint32_t sender_ms,
                            const void *payload,
                            uint16_t payload_len,
                            uint8_t *out,
                            size_t out_capacity,
                            size_t *out_len);

esp_err_t app_packet_decode(const uint8_t *data,
                            size_t data_len,
                            app_packet_view_t *view);

const char *app_msg_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif
