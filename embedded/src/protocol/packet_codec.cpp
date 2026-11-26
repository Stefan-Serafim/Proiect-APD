#include "packet_codec.h"
#include <string.h>

uint32_t app_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t app_crc32(const uint8_t *data, size_t len)
{
    return app_crc32_update(0u, data, len);
}

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
                            size_t *out_len)
{
    if (!out || !out_len || payload_len > APP_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > 0 && !payload) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total = sizeof(app_packet_header_t) + payload_len;
    if (total > out_capacity || total > APP_ESPNOW_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    app_packet_header_t hdr = {
        .magic = APP_PROTO_MAGIC,
        .version = APP_PROTO_VERSION,
        .type = type,
        .sender_id = sender_id,
        .flags = flags,
        .msg_id = msg_id,
        .sequence = sequence,
        .payload_length = payload_len,
        .sender_ms = sender_ms,
        .packet_crc32 = 0u,
    };

    memcpy(out, &hdr, sizeof(hdr));
    if (payload_len > 0) {
        memcpy(out + sizeof(hdr), payload, payload_len);
    }

    const uint32_t crc = app_crc32(out, total);
    memcpy(out + offsetof(app_packet_header_t, packet_crc32), &crc, sizeof(crc));
    *out_len = total;
    return ESP_OK;
}

esp_err_t app_packet_decode(const uint8_t *data,
                            size_t data_len,
                            app_packet_view_t *view)
{
    if (!data || !view || data_len < sizeof(app_packet_header_t) || data_len > APP_ESPNOW_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    app_packet_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != APP_PROTO_MAGIC || hdr.version != APP_PROTO_VERSION) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (hdr.payload_length > APP_MAX_PAYLOAD ||
        sizeof(app_packet_header_t) + hdr.payload_length != data_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tmp[APP_ESPNOW_FRAME_MAX];
    memcpy(tmp, data, data_len);
    memset(tmp + offsetof(app_packet_header_t, packet_crc32), 0, sizeof(uint32_t));
    const uint32_t computed = app_crc32(tmp, data_len);
    if (computed != hdr.packet_crc32) {
        return ESP_ERR_INVALID_CRC;
    }

    view->header = hdr;
    view->payload = data + sizeof(app_packet_header_t);
    return ESP_OK;
}

const char *app_msg_type_name(uint8_t type)
{
    switch (type) {
        case MSG_TYPE_DATA:  return "DATA";
        case MSG_TYPE_ACK:   return "ACK";
        case MSG_TYPE_PING:  return "PING";
        case MSG_TYPE_PONG:  return "PONG";
        case MSG_TYPE_HELLO: return "HELLO";
        default:             return "UNKNOWN";
    }
}
