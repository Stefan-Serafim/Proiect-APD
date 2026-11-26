#include "message_queue.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "message_queue";
#define HISTORY_FILE_PATH "/storage/history.bin"
#endif

static tx_queue_item_t s_tx_queue[MSG_QUEUE_CAPACITY];
static chat_message_t s_history[MSG_HISTORY_CAPACITY];
static size_t s_history_head = 0;
static size_t s_history_count = 0;
static bool s_storage_enabled = false;
static msg_event_cb_t s_event_cb = NULL;
static void *s_event_ctx = NULL;

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_mutex = NULL;
#endif

static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t len = 0;
    while (len < max_len && s[len] != '\0') {
        len++;
    }
    return len;
}

static void lock_queue(void)
{
#ifdef ESP_PLATFORM
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
#endif
}

static void unlock_queue(void)
{
#ifdef ESP_PLATFORM
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
#endif
}

static void notify_event(msg_event_type_t event, const chat_message_t *msg)
{
    if (s_event_cb) {
        s_event_cb(event, msg, s_event_ctx);
    }
}

#ifdef ESP_PLATFORM
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t count;
    uint16_t reserved;
} history_file_hdr_t;

#define HISTORY_FILE_MAGIC 0x48495331u

static void save_history_locked(void)
{
    if (!s_storage_enabled) {
        return;
    }
    FILE *f = fopen(HISTORY_FILE_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s for writing", HISTORY_FILE_PATH);
        return;
    }
    history_file_hdr_t hdr = {
        .magic = HISTORY_FILE_MAGIC,
        .count = (uint16_t)s_history_count,
        .reserved = 0,
    };
    fwrite(&hdr, sizeof(hdr), 1, f);
    const size_t start = (s_history_count < MSG_HISTORY_CAPACITY)
                             ? 0u
                             : s_history_head;
    for (size_t i = 0; i < s_history_count; ++i) {
        const size_t idx = (start + i) % MSG_HISTORY_CAPACITY;
        chat_message_t rec = s_history[idx];
        // Any interrupted in-flight outgoing messages from previous boot become FAILED
        if (rec.is_outgoing && (rec.status == MSG_STATUS_QUEUED || rec.status == MSG_STATUS_SENDING)) {
            rec.status = MSG_STATUS_FAILED;
        }
        fwrite(&rec, sizeof(rec), 1, f);
    }
    fclose(f);
}

static void load_history_locked(void)
{
    if (!s_storage_enabled) {
        return;
    }
    FILE *f = fopen(HISTORY_FILE_PATH, "rb");
    if (!f) {
        return;
    }
    history_file_hdr_t hdr = {};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != HISTORY_FILE_MAGIC || hdr.count > MSG_HISTORY_CAPACITY) {
        fclose(f);
        return;
    }
    s_history_head = 0;
    s_history_count = 0;
    for (size_t i = 0; i < hdr.count; ++i) {
        chat_message_t rec = {};
        if (fread(&rec, sizeof(rec), 1, f) != 1) {
            break;
        }
        rec.text[APP_MAX_PAYLOAD] = '\0';
        rec.sender_name[sizeof(rec.sender_name) - 1] = '\0';
        s_history[s_history_head] = rec;
        s_history_head = (s_history_head + 1u) % MSG_HISTORY_CAPACITY;
        s_history_count++;
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %u history messages from storage", (unsigned)s_history_count);
}
#endif

static void append_history_locked(const chat_message_t *msg)
{
    s_history[s_history_head] = *msg;
    s_history_head = (s_history_head + 1u) % MSG_HISTORY_CAPACITY;
    if (s_history_count < MSG_HISTORY_CAPACITY) {
        s_history_count++;
    }
#ifdef ESP_PLATFORM
    save_history_locked();
#endif
}

static chat_message_t *find_history_by_id_locked(uint32_t msg_id)
{
    for (size_t i = 0; i < s_history_count; ++i) {
        const size_t idx = (s_history_head + MSG_HISTORY_CAPACITY - 1u - i) % MSG_HISTORY_CAPACITY;
        if (s_history[idx].msg_id == msg_id && s_history[idx].is_outgoing) {
            return &s_history[idx];
        }
    }
    return NULL;
}

esp_err_t message_queue_init(bool enable_storage)
{
    memset(s_tx_queue, 0, sizeof(s_tx_queue));
    memset(s_history, 0, sizeof(s_history));
    s_history_head = 0;
    s_history_count = 0;
    s_storage_enabled = false;

#ifdef ESP_PLATFORM
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (enable_storage) {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/storage",
            .partition_label = "storage",
            .max_files = 4,
            .format_if_mount_failed = true,
        };
        const esp_err_t err = esp_vfs_spiffs_register(&conf);
        if (err == ESP_OK) {
            s_storage_enabled = true;
            lock_queue();
            load_history_locked();
            unlock_queue();
        } else {
            ESP_LOGW(TAG, "SPIFFS storage mount failed (%s); using RAM-only history", esp_err_to_name(err));
        }
    }
#else
    (void)enable_storage;
#endif
    return ESP_OK;
}

void message_queue_register_callback(msg_event_cb_t cb, void *ctx)
{
    s_event_cb = cb;
    s_event_ctx = ctx;
}

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
                                         uint32_t timestamp_ms)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = bounded_strlen(text, APP_MAX_PAYLOAD + 1);
    if (len == 0 || len > APP_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    lock_queue();
    tx_queue_item_t *slot = NULL;
    for (size_t i = 0; i < MSG_QUEUE_CAPACITY; ++i) {
        if (!s_tx_queue[i].used) {
            slot = &s_tx_queue[i];
            break;
        }
    }
    if (!slot) {
        unlock_queue();
#ifdef ESP_PLATFORM
        return ESP_ERR_NO_MEM;
#else
        return -1;
#endif
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->msg_id = msg_id;
    slot->sequence = sequence;
    if (has_dst_mac && dst_mac) {
        memcpy(slot->dst_mac, dst_mac, APP_MAC_LEN);
        slot->has_dst_mac = true;
    }
    slot->max_retries = max_retries ? max_retries : 1;
    slot->ack_timeout_ms = ack_timeout_ms ? ack_timeout_ms : 400;
    slot->text_len = (uint16_t)len;
    memcpy(slot->text, text, len);
    slot->text[len] = '\0';

    chat_message_t msg = {};
    msg.msg_id = msg_id;
    msg.sequence = sequence;
    msg.sender_id = self_id;
    msg.peer_id = peer_id;
    msg.is_outgoing = true;
    msg.status = MSG_STATUS_QUEUED;
    msg.retries_used = 0;
    msg.timestamp_ms = timestamp_ms;
    snprintf(msg.sender_name, sizeof(msg.sender_name), "%s", self_name ? self_name : "LOCAL");
    memcpy(msg.text, text, len);
    msg.text[len] = '\0';

    append_history_locked(&msg);
    unlock_queue();

    notify_event(MSG_EVENT_ADDED, &msg);
    return ESP_OK;
}

esp_err_t message_queue_add_incoming(uint32_t msg_id,
                                     uint16_t sequence,
                                     uint8_t sender_id,
                                     const char *sender_name,
                                     uint8_t self_id,
                                     int8_t rssi,
                                     uint32_t timestamp_ms,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    if (!payload || payload_len == 0 || payload_len > APP_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    chat_message_t msg = {};
    msg.msg_id = msg_id;
    msg.sequence = sequence;
    msg.sender_id = sender_id;
    msg.peer_id = self_id;
    msg.is_outgoing = false;
    msg.status = MSG_STATUS_RECEIVED;
    msg.rssi = rssi;
    msg.timestamp_ms = timestamp_ms;
    snprintf(msg.sender_name, sizeof(msg.sender_name), "%s", sender_name ? sender_name : "PEER");
    memcpy(msg.text, payload, payload_len);
    msg.text[payload_len] = '\0';

    lock_queue();
    append_history_locked(&msg);
    unlock_queue();

    notify_event(MSG_EVENT_ADDED, &msg);
    return ESP_OK;
}

bool message_queue_get_next_tx(tx_queue_item_t *out)
{
    if (!out) {
        return false;
    }
    bool found = false;
    lock_queue();
    // Pick oldest active item (FIFO stop-and-wait ARQ ordering)
    uint32_t best_id = UINT32_MAX;
    size_t best_idx = 0;
    for (size_t i = 0; i < MSG_QUEUE_CAPACITY; ++i) {
        if (s_tx_queue[i].used && (!found || s_tx_queue[i].msg_id < best_id)) {
            best_id = s_tx_queue[i].msg_id;
            best_idx = i;
            found = true;
        }
    }
    if (found) {
        *out = s_tx_queue[best_idx];
    }
    unlock_queue();
    return found;
}

void message_queue_mark_tx_attempt(uint32_t msg_id, int64_t now_us, const uint8_t dst_mac[APP_MAC_LEN], bool has_dst_mac)
{
    chat_message_t updated = {};
    bool notify = false;

    lock_queue();
    for (size_t i = 0; i < MSG_QUEUE_CAPACITY; ++i) {
        if (s_tx_queue[i].used && s_tx_queue[i].msg_id == msg_id) {
            if (s_tx_queue[i].attempts == 0) {
                s_tx_queue[i].first_tx_us = now_us;
            }
            s_tx_queue[i].attempts++;
            s_tx_queue[i].last_tx_us = now_us;
            s_tx_queue[i].waiting_ack = true;
            if (has_dst_mac && dst_mac) {
                memcpy(s_tx_queue[i].dst_mac, dst_mac, APP_MAC_LEN);
                s_tx_queue[i].has_dst_mac = true;
            }

            chat_message_t *h = find_history_by_id_locked(msg_id);
            if (h) {
                h->status = MSG_STATUS_SENDING;
                h->retries_used = (uint8_t)(s_tx_queue[i].attempts > 0 ? s_tx_queue[i].attempts - 1u : 0u);
                updated = *h;
                notify = true;
            }
            break;
        }
    }
    unlock_queue();

    if (notify) {
        notify_event(MSG_EVENT_STATUS_CHANGED, &updated);
    }
}

bool message_queue_complete_ack(uint32_t msg_id, int64_t now_us)
{
    chat_message_t updated = {};
    bool completed = false;

    lock_queue();
    for (size_t i = 0; i < MSG_QUEUE_CAPACITY; ++i) {
        if (s_tx_queue[i].used && s_tx_queue[i].msg_id == msg_id) {
            const int64_t diff_us = (now_us > s_tx_queue[i].first_tx_us) ? (now_us - s_tx_queue[i].first_tx_us) : 0;
            const uint32_t rtt_ms = (uint32_t)((diff_us + 500) / 1000);
            const uint8_t retries = (uint8_t)(s_tx_queue[i].attempts > 0 ? s_tx_queue[i].attempts - 1u : 0u);
            memset(&s_tx_queue[i], 0, sizeof(s_tx_queue[i]));

            chat_message_t *h = find_history_by_id_locked(msg_id);
            if (h) {
                h->status = MSG_STATUS_DELIVERED;
                h->retries_used = retries;
                h->rtt_ms = rtt_ms;
                updated = *h;
            }
#ifdef ESP_PLATFORM
            save_history_locked();
#endif
            completed = true;
            break;
        }
    }
    unlock_queue();

    if (completed) {
        notify_event(MSG_EVENT_STATUS_CHANGED, &updated);
    }
    return completed;
}

bool message_queue_mark_failed(uint32_t msg_id)
{
    chat_message_t updated = {};
    bool failed = false;

    lock_queue();
    for (size_t i = 0; i < MSG_QUEUE_CAPACITY; ++i) {
        if (s_tx_queue[i].used && s_tx_queue[i].msg_id == msg_id) {
            const uint8_t retries = (uint8_t)(s_tx_queue[i].attempts > 0 ? s_tx_queue[i].attempts - 1u : 0u);
            memset(&s_tx_queue[i], 0, sizeof(s_tx_queue[i]));

            chat_message_t *h = find_history_by_id_locked(msg_id);
            if (h) {
                h->status = MSG_STATUS_FAILED;
                h->retries_used = retries;
                updated = *h;
            }
#ifdef ESP_PLATFORM
            save_history_locked();
#endif
            failed = true;
            break;
        }
    }
    unlock_queue();

    if (failed) {
        notify_event(MSG_EVENT_STATUS_CHANGED, &updated);
    }
    return failed;
}

size_t message_queue_get_history(chat_message_t *out, size_t max_items)
{
    if (!out || max_items == 0) {
        return 0;
    }
    lock_queue();
    const size_t total = s_history_count;
    const size_t count = total < max_items ? total : max_items;
    const size_t skip = total - count;
    const size_t start = (s_history_count < MSG_HISTORY_CAPACITY) ? 0u : s_history_head;
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (start + skip + i) % MSG_HISTORY_CAPACITY;
        out[i] = s_history[idx];
    }
    unlock_queue();
    return count;
}

void message_queue_clear_history(void)
{
    lock_queue();
    memset(s_history, 0, sizeof(s_history));
    s_history_head = 0;
    s_history_count = 0;
#ifdef ESP_PLATFORM
    save_history_locked();
#endif
    unlock_queue();
    notify_event(MSG_EVENT_HISTORY_CLEARED, NULL);
}

size_t message_queue_pending_count(void)
{
    size_t count = 0;
    lock_queue();
    for (size_t i = 0; i < MSG_QUEUE_CAPACITY; ++i) {
        if (s_tx_queue[i].used) {
            count++;
        }
    }
    unlock_queue();
    return count;
}

const char *msg_status_name(msg_status_t status)
{
    switch (status) {
        case MSG_STATUS_QUEUED:    return "queued";
        case MSG_STATUS_SENDING:   return "sending";
        case MSG_STATUS_DELIVERED: return "delivered";
        case MSG_STATUS_FAILED:    return "failed";
        case MSG_STATUS_RECEIVED:  return "received";
        default:                   return "unknown";
    }
}
