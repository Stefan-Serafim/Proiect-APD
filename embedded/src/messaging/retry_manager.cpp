#include "retry_manager.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "dedup_filter.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define PONG_BIT BIT0
#define PEER_OFFLINE_MS 6500u

static const char *TAG = "retry_mgr";

static uint8_t s_self_id = 1;
static char s_self_name[16] = "NODE";
static uint16_t s_boot_epoch = 1;
static uint16_t s_next_seq = 1;

static chat_peer_t s_peers[CHAT_MAX_PEERS];
static uint8_t s_selected_mac[APP_MAC_LEN];
static bool s_have_selected = false;

static dedup_filter_t s_dedup;
static SemaphoreHandle_t s_state_mutex = NULL;
static EventGroupHandle_t s_test_events = NULL;
static TaskHandle_t s_retry_task = NULL;
static TaskHandle_t s_test_task = NULL;

static peer_event_cb_t s_peer_cb = NULL;
static void *s_peer_ctx = NULL;
static test_event_cb_t s_test_cb = NULL;
static void *s_test_ctx = NULL;

static messaging_stats_t s_stats = {};
static unsigned s_loss_percent = 0;
static bool s_drop_next_ack = false;

// Burst / Ping-Pong test state
static volatile uint32_t s_wait_test_id = 0;
static volatile uint32_t s_wait_pong_idx = 0;
static volatile int64_t s_pong_rx_us = 0;
static volatile int8_t s_pong_rssi = 0;
static test_summary_t s_test_summary = {};
static test_sample_t s_test_samples[TEST_MAX_SAMPLES];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t alloc_seq(void)
{
    uint16_t v = s_next_seq++;
    if (s_next_seq == 0) {
        s_next_seq = 1;
    }
    return v;
}

static uint32_t make_msg_id(uint16_t seq)
{
    return (((uint32_t)s_boot_epoch) << 16) | (uint32_t)seq;
}

static void load_and_bump_boot_epoch(void)
{
    nvs_handle_t h;
    if (nvs_open("msg_state", NVS_READWRITE, &h) == ESP_OK) {
        uint16_t epoch = 0;
        (void)nvs_get_u16(h, "epoch", &epoch);
        epoch++;
        if (epoch == 0) {
            epoch = 1;
        }
        s_boot_epoch = epoch;
        (void)nvs_set_u16(h, "epoch", epoch);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static esp_err_t send_frame(const uint8_t dst[APP_MAC_LEN],
                            uint8_t type,
                            uint32_t msg_id,
                            uint16_t seq,
                            const void *payload,
                            uint16_t payload_len)
{
    uint8_t frame[APP_ESPNOW_FRAME_MAX];
    size_t frame_len = 0;
    const esp_err_t err = app_packet_encode(
        type, msg_id, seq, s_self_id, 0, now_ms(),
        payload, payload_len, frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) {
        return err;
    }
    return espnow_link_send(dst, frame, frame_len);
}

static void send_ack(const uint8_t dst[APP_MAC_LEN], uint32_t ack_msg_id, uint16_t ack_seq, uint8_t acked_type)
{
    app_ack_payload_t ack = {
        .ack_msg_id = ack_msg_id,
        .ack_sequence = ack_seq,
        .acked_type = acked_type,
        .status = 0,
    };
    const uint16_t seq = alloc_seq();
    (void)send_frame(dst, MSG_TYPE_ACK, make_msg_id(seq), seq, &ack, sizeof(ack));
}

static chat_peer_t *upsert_peer_locked(const uint8_t mac[APP_MAC_LEN], uint8_t node_id, int8_t rssi, bool *became_online)
{
    chat_peer_t *free_slot = NULL;
    for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
        if (s_peers[i].used && memcmp(s_peers[i].mac, mac, APP_MAC_LEN) == 0) {
            if (became_online) {
                *became_online = !s_peers[i].online;
            }
            s_peers[i].node_id = node_id;
            s_peers[i].rssi = rssi;
            s_peers[i].last_seen_ms = now_ms();
            s_peers[i].online = true;
            return &s_peers[i];
        }
        if (!s_peers[i].used && !free_slot) {
            free_slot = &s_peers[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->online = true;
        free_slot->node_id = node_id;
        memcpy(free_slot->mac, mac, APP_MAC_LEN);
        free_slot->rssi = rssi;
        free_slot->last_seen_ms = now_ms();
        snprintf(free_slot->name, sizeof(free_slot->name), "NODE_%u", node_id);
        if (became_online) {
            *became_online = true;
        }
        if (!s_have_selected) {
            memcpy(s_selected_mac, mac, APP_MAC_LEN);
            s_have_selected = true;
        }
        return free_slot;
    }
    return NULL;
}

static void upsert_peer(const uint8_t mac[APP_MAC_LEN], uint8_t node_id, int8_t rssi)
{
    chat_peer_t snapshot = {};
    bool notify = false;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    chat_peer_t *p = upsert_peer_locked(mac, node_id, rssi, &notify);
    if (p) {
        snapshot = *p;
    }
    xSemaphoreGive(s_state_mutex);

    (void)espnow_link_add_peer(mac);
    if (notify && s_peer_cb) {
        s_peer_cb(&snapshot, s_peer_ctx);
    }
}

static void retry_manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        tx_queue_item_t item;
        if (message_queue_get_next_tx(&item)) {
            const int64_t now_us = esp_timer_get_time();

            uint8_t target_mac[APP_MAC_LEN] = {};
            bool have_mac = item.has_dst_mac;
            if (have_mac) {
                memcpy(target_mac, item.dst_mac, APP_MAC_LEN);
            } else {
                chat_peer_t peer;
                if (retry_manager_get_active_peer(&peer)) {
                    memcpy(target_mac, peer.mac, APP_MAC_LEN);
                    have_mac = true;
                }
            }

            if (!item.waiting_ack) {
                // First transmission
                if (have_mac) {
                    (void)send_frame(target_mac, MSG_TYPE_DATA, item.msg_id, item.sequence, item.text, item.text_len);
                }
                message_queue_mark_tx_attempt(item.msg_id, now_us, target_mac, have_mac);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_stats.tx_sent++;
                xSemaphoreGive(s_state_mutex);
                printf("[EMB-2 TX #%u id=0x%08X] attempt 1/%u\n",
                       item.sequence, (unsigned)item.msg_id, item.max_retries);
            } else {
                const int64_t elapsed_ms = (now_us - item.last_tx_us) / 1000;
                if (elapsed_ms >= (int64_t)item.ack_timeout_ms) {
                    if (item.attempts >= item.max_retries) {
                        (void)message_queue_mark_failed(item.msg_id);
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        s_stats.tx_failed++;
                        xSemaphoreGive(s_state_mutex);
                        printf("[EMB-2 FAIL #%u id=0x%08X] max retries (%u) reached\n",
                               item.sequence, (unsigned)item.msg_id, item.max_retries);
                    } else {
                        if (have_mac) {
                            (void)send_frame(target_mac, MSG_TYPE_DATA, item.msg_id, item.sequence, item.text, item.text_len);
                        }
                        message_queue_mark_tx_attempt(item.msg_id, now_us, target_mac, have_mac);
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        s_stats.tx_retransmissions++;
                        xSemaphoreGive(s_state_mutex);
                        printf("[EMB-2 RETRY #%u id=0x%08X] attempt %u/%u\n",
                               item.sequence, (unsigned)item.msg_id, item.attempts + 1u, item.max_retries);
                    }
                }
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}

static int cmp_float(const void *a, const void *b)
{
    const float fa = *(const float *)a;
    const float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void compute_test_stats(test_summary_t *sum, const test_sample_t *samples, size_t n, float elapsed_s)
{
    static float rtts[TEST_MAX_SAMPLES];
    size_t ok_n = 0;
    double rtt_sum = 0.0;
    double rssi_sum = 0.0;
    float min_r = 1e9f;
    float max_r = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        if (samples[i].acked) {
            const float r = samples[i].rtt_ms;
            rtts[ok_n++] = r;
            rtt_sum += r;
            rssi_sum += samples[i].rssi;
            if (r < min_r) min_r = r;
            if (r > max_r) max_r = r;
        }
    }

    sum->duration_s = elapsed_s;
    sum->sample_count = n;
    if (ok_n == 0) {
        sum->avg_rtt_ms = 0.0f;
        sum->min_rtt_ms = 0.0f;
        sum->max_rtt_ms = 0.0f;
        sum->median_rtt_ms = 0.0f;
        sum->p95_rtt_ms = 0.0f;
        sum->stddev_rtt_ms = 0.0f;
        sum->avg_rssi = 0.0f;
        return;
    }

    qsort(rtts, ok_n, sizeof(float), cmp_float);
    const float mean = (float)(rtt_sum / (double)ok_n);
    double var_sum = 0.0;
    for (size_t i = 0; i < ok_n; ++i) {
        const double d = rtts[i] - mean;
        var_sum += d * d;
    }

    sum->avg_rtt_ms = mean;
    sum->min_rtt_ms = min_r;
    sum->max_rtt_ms = max_r;
    sum->median_rtt_ms = (ok_n % 2u == 1u)
                             ? rtts[ok_n / 2u]
                             : 0.5f * (rtts[ok_n / 2u - 1u] + rtts[ok_n / 2u]);
    size_t p95_idx = (size_t)ceil(0.95 * (double)ok_n);
    if (p95_idx > 0) p95_idx--;
    if (p95_idx >= ok_n) p95_idx = ok_n - 1u;
    sum->p95_rtt_ms = rtts[p95_idx];
    sum->stddev_rtt_ms = (float)sqrt(var_sum / (double)ok_n);
    sum->avg_rssi = (float)(rssi_sum / (double)ok_n);
}

typedef struct {
    unsigned count;
    bool reliable_mode;
} test_params_t;

static void test_runner_task(void *arg)
{
    test_params_t *params = (test_params_t *)arg;
    const unsigned count = params->count;
    const bool reliable = params->reliable_mode;
    free(params);

    chat_peer_t peer;
    if (!retry_manager_get_active_peer(&peer)) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_test_summary.running = false;
        xSemaphoreGive(s_state_mutex);
        if (s_test_cb) {
            s_test_cb(&s_test_summary, true, s_test_ctx);
        }
        s_test_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const uint32_t test_id = esp_random();
    const int64_t t0 = esp_timer_get_time();
    const unsigned max_attempts = reliable ? (unsigned)CONFIG_CHAT_MAX_RETRIES : 1u;
    const unsigned wait_ms = reliable ? (unsigned)CONFIG_CHAT_ACK_TIMEOUT_MS : 300u;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memset(&s_test_summary, 0, sizeof(s_test_summary));
    memset(s_test_samples, 0, sizeof(s_test_samples));
    s_test_summary.running = true;
    s_test_summary.test_id = test_id;
    s_test_summary.total_count = count;
    xSemaphoreGive(s_state_mutex);

    for (unsigned i = 0; i < count; ++i) {
        bool ok = false;
        uint8_t retries = 0;
        float rtt_ms = 0.0f;
        int8_t rssi = 0;
        const int64_t pkt_start = esp_timer_get_time();

        for (unsigned attempt = 1; attempt <= max_attempts; ++attempt) {
            const uint16_t seq = alloc_seq();
            app_ping_payload_t ping = {
                .test_id = test_id,
                .seq_index = i,
                .token = esp_random(),
                .tx_ms = now_ms(),
            };
            s_wait_test_id = test_id;
            s_wait_pong_idx = i;
            s_pong_rx_us = 0;
            xEventGroupClearBits(s_test_events, PONG_BIT);

            if (send_frame(peer.mac, MSG_TYPE_PING, make_msg_id(seq), seq, &ping, sizeof(ping)) == ESP_OK) {
                const EventBits_t bits = xEventGroupWaitBits(
                    s_test_events, PONG_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
                if (bits & PONG_BIT) {
                    ok = true;
                    retries = (uint8_t)(attempt - 1u);
                    rtt_ms = (float)(s_pong_rx_us - pkt_start) / 1000.0f;
                    rssi = s_pong_rssi;
                    break;
                }
            }
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_test_summary.completed_count = i + 1u;
        if (ok) {
            if (retries == 0) {
                s_test_summary.acked_first_try++;
            } else {
                s_test_summary.retried_ok++;
            }
        } else {
            s_test_summary.lost++;
        }
        if (i < TEST_MAX_SAMPLES) {
            s_test_samples[i].seq_index = i + 1u;
            s_test_samples[i].rtt_ms = rtt_ms;
            s_test_samples[i].acked = ok;
            s_test_samples[i].retries = retries;
            s_test_samples[i].rssi = rssi;
        }
        const size_t stored = (i + 1u) < TEST_MAX_SAMPLES ? (i + 1u) : TEST_MAX_SAMPLES;
        const float elapsed_s = (float)(esp_timer_get_time() - t0) / 1000000.0f;
        compute_test_stats(&s_test_summary, s_test_samples, stored, elapsed_s);
        test_summary_t snap = s_test_summary;
        xSemaphoreGive(s_state_mutex);

        if (s_test_cb && (((i + 1u) % 5u) == 0u || (i + 1u) == count)) {
            s_test_cb(&snap, false, s_test_ctx);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_test_summary.running = false;
    test_summary_t final_snap = s_test_summary;
    xSemaphoreGive(s_state_mutex);

    if (s_test_cb) {
        s_test_cb(&final_snap, true, s_test_ctx);
    }

    s_test_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t retry_manager_init(uint8_t self_id, const char *self_name)
{
    s_self_id = self_id;
    snprintf(s_self_name, sizeof(s_self_name), "%s", self_name ? self_name : "NODE");
    memset(s_peers, 0, sizeof(s_peers));
    memset(&s_stats, 0, sizeof(s_stats));
    dedup_filter_init(&s_dedup);
    load_and_bump_boot_epoch();

    s_state_mutex = xSemaphoreCreateMutex();
    s_test_events = xEventGroupCreate();
    if (!s_state_mutex || !s_test_events) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(retry_manager_task, "retry_mgr", 4096, NULL, 7, &s_retry_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "EMB-2 retry manager ready (epoch=%u, timeout=%dms, max_retries=%d)",
             (unsigned)s_boot_epoch, CONFIG_CHAT_ACK_TIMEOUT_MS, CONFIG_CHAT_MAX_RETRIES);
    return ESP_OK;
}

void retry_manager_register_peer_cb(peer_event_cb_t cb, void *ctx)
{
    s_peer_cb = cb;
    s_peer_ctx = ctx;
}

void retry_manager_register_test_cb(test_event_cb_t cb, void *ctx)
{
    s_test_cb = cb;
    s_test_ctx = ctx;
}

esp_err_t retry_manager_send_text(const char *text, uint32_t *out_msg_id)
{
    chat_peer_t peer = {};
    const bool have = retry_manager_get_active_peer(&peer);
    return retry_manager_send_text_to(have ? peer.mac : NULL, text, out_msg_id);
}

esp_err_t retry_manager_send_text_to(const uint8_t dst_mac[APP_MAC_LEN], const char *text, uint32_t *out_msg_id)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t seq = alloc_seq();
    const uint32_t msg_id = make_msg_id(seq);
    uint8_t peer_id = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (dst_mac) {
        for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
            if (s_peers[i].used && memcmp(s_peers[i].mac, dst_mac, APP_MAC_LEN) == 0) {
                peer_id = s_peers[i].node_id;
                break;
            }
        }
    }
    xSemaphoreGive(s_state_mutex);

    const esp_err_t err = message_queue_enqueue_outgoing(
        msg_id, seq, s_self_id, s_self_name, peer_id,
        dst_mac, dst_mac != NULL, text,
        CONFIG_CHAT_MAX_RETRIES, CONFIG_CHAT_ACK_TIMEOUT_MS, now_ms());
    if (err == ESP_OK) {
        if (out_msg_id) {
            *out_msg_id = msg_id;
        }
        if (s_retry_task) {
            xTaskNotifyGive(s_retry_task);
        }
    }
    return err;
}

void retry_manager_send_hello(void)
{
    app_hello_payload_t hello = {};
    snprintf(hello.name, sizeof(hello.name), "%s", s_self_name);
    hello.channel = espnow_link_get_channel();
    uint8_t frame[APP_ESPNOW_FRAME_MAX];
    size_t frame_len = 0;
    const uint16_t seq = alloc_seq();
    if (app_packet_encode(MSG_TYPE_HELLO, make_msg_id(seq), seq, s_self_id, 0, now_ms(),
                          &hello, sizeof(hello), frame, sizeof(frame), &frame_len) == ESP_OK) {
        (void)espnow_link_send_broadcast(frame, frame_len);
    }
}

void retry_manager_handle_packet(const uint8_t src_mac[APP_MAC_LEN], int8_t rssi, const app_packet_view_t *packet)
{
    if (!src_mac || !packet || packet->header.sender_id == s_self_id) {
        return;
    }

    upsert_peer(src_mac, packet->header.sender_id, rssi);

    switch (packet->header.type) {
        case MSG_TYPE_HELLO: {
            if (packet->header.payload_length == sizeof(app_hello_payload_t)) {
                app_hello_payload_t hello;
                memcpy(&hello, packet->payload, sizeof(hello));
                hello.name[sizeof(hello.name) - 1] = '\0';
                chat_peer_t snapshot = {};
                bool notify = false;
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                chat_peer_t *p = upsert_peer_locked(src_mac, packet->header.sender_id, rssi, &notify);
                if (p) {
                    if (strcmp(p->name, hello.name) != 0) {
                        snprintf(p->name, sizeof(p->name), "%s", hello.name);
                        notify = true;
                    }
                    snapshot = *p;
                }
                xSemaphoreGive(s_state_mutex);
                if (notify && s_peer_cb) {
                    s_peer_cb(&snapshot, s_peer_ctx);
                }
            }
            break;
        }
        case MSG_TYPE_ACK: {
            if (packet->header.payload_length == sizeof(app_ack_payload_t)) {
                app_ack_payload_t ack;
                memcpy(&ack, packet->payload, sizeof(ack));
                if (ack.acked_type == MSG_TYPE_DATA) {
                    tx_queue_item_t cur;
                    uint8_t attempts = 1;
                    if (message_queue_get_next_tx(&cur) && cur.msg_id == ack.ack_msg_id) {
                        attempts = cur.attempts;
                    }
                    if (message_queue_complete_ack(ack.ack_msg_id, esp_timer_get_time())) {
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        if (attempts <= 1) {
                            s_stats.tx_acked_first++;
                        } else {
                            s_stats.tx_retried_ack++;
                        }
                        xSemaphoreGive(s_state_mutex);
                        printf("[EMB-2 ACK id=0x%08X #%u]\n", (unsigned)ack.ack_msg_id, ack.ack_sequence);
                        if (s_retry_task) {
                            xTaskNotifyGive(s_retry_task);
                        }
                    }
                }
            }
            break;
        }
        case MSG_TYPE_DATA: {
            if (s_loss_percent && (esp_random() % 100u) < s_loss_percent) {
                printf("[SIM DROP] DATA id=0x%08X from node %u\n",
                       (unsigned)packet->header.msg_id, packet->header.sender_id);
                break;
            }

            char sender_name[16] = {};
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const bool is_dup = dedup_filter_check_and_insert(&s_dedup, packet->header.sender_id, packet->header.msg_id);
            if (is_dup) {
                s_stats.rx_duplicates++;
            } else {
                s_stats.rx_received++;
            }
            for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
                if (s_peers[i].used && s_peers[i].node_id == packet->header.sender_id) {
                    snprintf(sender_name, sizeof(sender_name), "%s", s_peers[i].name);
                    break;
                }
            }
            xSemaphoreGive(s_state_mutex);

            if (!sender_name[0]) {
                snprintf(sender_name, sizeof(sender_name), "NODE_%u", packet->header.sender_id);
            }

            if (!is_dup) {
                (void)message_queue_add_incoming(
                    packet->header.msg_id,
                    packet->header.sequence,
                    packet->header.sender_id,
                    sender_name,
                    s_self_id,
                    rssi,
                    now_ms(),
                    packet->payload,
                    packet->header.payload_length);
                printf("[RX %s id=0x%08X rssi=%d] %.*s\n",
                       sender_name, (unsigned)packet->header.msg_id, rssi,
                       (int)packet->header.payload_length, (const char *)packet->payload);
            } else {
                printf("[DEDUP id=0x%08X] suppressed duplicate, re-sending ACK\n",
                       (unsigned)packet->header.msg_id);
            }

            if (s_drop_next_ack) {
                s_drop_next_ack = false;
                printf("[SIM DROP] skipped first ACK for id=0x%08X\n", (unsigned)packet->header.msg_id);
            } else {
                send_ack(src_mac, packet->header.msg_id, packet->header.sequence, MSG_TYPE_DATA);
            }
            break;
        }
        case MSG_TYPE_PING: {
            if (s_loss_percent && (esp_random() % 100u) < s_loss_percent) {
                break;
            }
            const uint16_t seq = alloc_seq();
            (void)send_frame(src_mac, MSG_TYPE_PONG, make_msg_id(seq), seq,
                             packet->payload, packet->header.payload_length);
            break;
        }
        case MSG_TYPE_PONG: {
            if (packet->header.payload_length == sizeof(app_ping_payload_t)) {
                app_ping_payload_t pong;
                memcpy(&pong, packet->payload, sizeof(pong));
                if (pong.test_id == s_wait_test_id && pong.seq_index == s_wait_pong_idx) {
                    s_pong_rx_us = esp_timer_get_time();
                    s_pong_rssi = rssi;
                    xEventGroupSetBits(s_test_events, PONG_BIT);
                }
            }
            break;
        }
        default:
            break;
    }
}

void retry_manager_check_peer_timeouts(void)
{
    const uint32_t t = now_ms();
    chat_peer_t changed[CHAT_MAX_PEERS];
    size_t changed_n = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
        if (s_peers[i].used && s_peers[i].online && (t - s_peers[i].last_seen_ms) > PEER_OFFLINE_MS) {
            s_peers[i].online = false;
            changed[changed_n++] = s_peers[i];
        }
    }
    xSemaphoreGive(s_state_mutex);

    if (s_peer_cb) {
        for (size_t i = 0; i < changed_n; ++i) {
            s_peer_cb(&changed[i], s_peer_ctx);
        }
    }
}

esp_err_t retry_manager_start_test(unsigned count, bool reliable_mode)
{
    if (count == 0) count = 100;
    if (count > TEST_MAX_SAMPLES) count = TEST_MAX_SAMPLES;

    if (s_test_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    chat_peer_t peer;
    if (!retry_manager_get_active_peer(&peer)) {
        return ESP_ERR_NOT_FOUND;
    }

    test_params_t *params = (test_params_t *)malloc(sizeof(test_params_t));
    if (!params) {
        return ESP_ERR_NO_MEM;
    }
    params->count = count;
    params->reliable_mode = reliable_mode;

    if (xTaskCreate(test_runner_task, "link_test", 4096, params, 5, &s_test_task) != pdPASS) {
        free(params);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void retry_manager_get_test_summary(test_summary_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_test_summary;
    xSemaphoreGive(s_state_mutex);
}

size_t retry_manager_get_test_samples(test_sample_t *out, size_t max_samples)
{
    if (!out || max_samples == 0) return 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const size_t n = s_test_summary.sample_count < max_samples ? s_test_summary.sample_count : max_samples;
    memcpy(out, s_test_samples, n * sizeof(test_sample_t));
    xSemaphoreGive(s_state_mutex);
    return n;
}

size_t retry_manager_export_csv(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    size_t pos = (size_t)snprintf(buf, buf_size, "seq,acked,rtt_ms,retries,rssi_dbm\n");
    const size_t n = s_test_summary.sample_count;
    for (size_t i = 0; i < n && pos + 64 < buf_size; ++i) {
        const test_sample_t *s = &s_test_samples[i];
        pos += (size_t)snprintf(buf + pos, buf_size - pos, "%u,%u,%.2f,%u,%d\n",
                                (unsigned)s->seq_index, s->acked ? 1u : 0u,
                                (double)s->rtt_ms, (unsigned)s->retries, (int)s->rssi);
    }
    xSemaphoreGive(s_state_mutex);
    return pos;
}

size_t retry_manager_get_peers(chat_peer_t *out, size_t max_peers)
{
    if (!out || max_peers == 0) return 0;
    size_t n = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHAT_MAX_PEERS && n < max_peers; ++i) {
        if (s_peers[i].used) out[n++] = s_peers[i];
    }
    xSemaphoreGive(s_state_mutex);
    return n;
}

bool retry_manager_get_active_peer(chat_peer_t *out)
{
    bool found = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_have_selected) {
        for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
            if (s_peers[i].used && memcmp(s_peers[i].mac, s_selected_mac, APP_MAC_LEN) == 0) {
                if (out) *out = s_peers[i];
                found = true;
                break;
            }
        }
    }
    if (!found) {
        for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
            if (s_peers[i].used) {
                if (out) *out = s_peers[i];
                memcpy(s_selected_mac, s_peers[i].mac, APP_MAC_LEN);
                s_have_selected = true;
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_state_mutex);
    return found;
}

bool retry_manager_select_peer_by_id(uint8_t node_id)
{
    bool found = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHAT_MAX_PEERS; ++i) {
        if (s_peers[i].used && s_peers[i].node_id == node_id) {
            memcpy(s_selected_mac, s_peers[i].mac, APP_MAC_LEN);
            s_have_selected = true;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return found;
}

void retry_manager_get_stats(messaging_stats_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_stats;
    out->sim_loss_pct = s_loss_percent;
    xSemaphoreGive(s_state_mutex);
}

void retry_manager_set_loss_percent(unsigned percent)
{
    if (percent > 100) percent = 100;
    s_loss_percent = percent;
}

unsigned retry_manager_get_loss_percent(void)
{
    return s_loss_percent;
}

void retry_manager_drop_next_ack(void)
{
    s_drop_next_ack = true;
}
