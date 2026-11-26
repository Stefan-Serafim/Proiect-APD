#include "ws_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_log.h"
#include "espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WS_MAX_CLIENTS 8

static const char *TAG = "ws_handler";
static httpd_handle_t s_server = NULL;
static int s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

static void add_client(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    bool exists = false;
    int free_idx = -1;
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (s_clients[i] == fd) {
            exists = true;
            break;
        }
        if (s_clients[i] < 0 && free_idx < 0) {
            free_idx = i;
        }
    }
    if (!exists && free_idx >= 0) {
        s_clients[free_idx] = fd;
    }
    xSemaphoreGive(s_ws_mutex);
}

static void remove_client(int fd)
{
    if (!s_ws_mutex) return;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (s_clients[i] == fd) {
            s_clients[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void send_json_to_fd(int fd, const char *json)
{
    if (!s_server || fd < 0 || !json) return;
    httpd_ws_frame_t pkt = {};
    pkt.payload = (uint8_t *)json;
    pkt.len = strlen(json);
    pkt.type = HTTPD_WS_TYPE_TEXT;
    if (httpd_ws_send_frame_async(s_server, fd, &pkt) != ESP_OK) {
        remove_client(fd);
    }
}

static void broadcast_json(const char *json)
{
    if (!s_server || !s_ws_mutex || !json) return;
    int fds[WS_MAX_CLIENTS];
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    memcpy(fds, s_clients, sizeof(fds));
    xSemaphoreGive(s_ws_mutex);

    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (fds[i] >= 0) {
            send_json_to_fd(fds[i], json);
        }
    }
}

static cJSON *message_to_json(const chat_message_t *m)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj || !m) return obj;
    cJSON_AddNumberToObject(obj, "msg_id", (double)m->msg_id);
    cJSON_AddNumberToObject(obj, "sequence", m->sequence);
    cJSON_AddNumberToObject(obj, "sender_id", m->sender_id);
    cJSON_AddStringToObject(obj, "sender_name", m->sender_name);
    cJSON_AddBoolToObject(obj, "is_outgoing", m->is_outgoing);
    cJSON_AddStringToObject(obj, "status", msg_status_name(m->status));
    cJSON_AddNumberToObject(obj, "retries", m->retries_used);
    cJSON_AddNumberToObject(obj, "rtt_ms", m->rtt_ms);
    cJSON_AddNumberToObject(obj, "rssi", m->rssi);
    cJSON_AddNumberToObject(obj, "timestamp_ms", (double)m->timestamp_ms);
    cJSON_AddStringToObject(obj, "text", m->text);
    return obj;
}

static cJSON *test_summary_to_json(const test_summary_t *s, const char *type_name)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj || !s) return obj;
    cJSON_AddStringToObject(obj, "type", type_name);
    cJSON_AddBoolToObject(obj, "running", s->running);
    cJSON_AddNumberToObject(obj, "total", s->total_count);
    cJSON_AddNumberToObject(obj, "completed", s->completed_count);
    cJSON_AddNumberToObject(obj, "acked_first", s->acked_first_try);
    cJSON_AddNumberToObject(obj, "retried_ok", s->retried_ok);
    cJSON_AddNumberToObject(obj, "lost", s->lost);
    const float loss_pct = s->completed_count ? (100.0f * (float)s->lost / (float)s->completed_count) : 0.0f;
    cJSON_AddNumberToObject(obj, "loss_pct", loss_pct);
    cJSON_AddNumberToObject(obj, "avg_rtt_ms", s->avg_rtt_ms);
    cJSON_AddNumberToObject(obj, "min_rtt_ms", s->min_rtt_ms);
    cJSON_AddNumberToObject(obj, "max_rtt_ms", s->max_rtt_ms);
    cJSON_AddNumberToObject(obj, "median_rtt_ms", s->median_rtt_ms);
    cJSON_AddNumberToObject(obj, "p95_rtt_ms", s->p95_rtt_ms);
    cJSON_AddNumberToObject(obj, "stddev_rtt_ms", s->stddev_rtt_ms);
    cJSON_AddNumberToObject(obj, "avg_rssi", s->avg_rssi);
    cJSON_AddNumberToObject(obj, "duration_s", s->duration_s);
    cJSON_AddNumberToObject(obj, "sample_count", (double)s->sample_count);
    return obj;
}

char *ws_build_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddNumberToObject(root, "node_id", CONFIG_APP_NODE_ID);
    cJSON_AddStringToObject(root, "node_name", CONFIG_APP_NODE_NAME);
    cJSON_AddStringToObject(root, "ap_ssid", CONFIG_APP_AP_SSID);
    cJSON_AddNumberToObject(root, "channel", espnow_link_get_channel());
    cJSON_AddNumberToObject(root, "ap_clients", espnow_link_get_ap_client_count());
    cJSON_AddNumberToObject(root, "pending_queue", (double)message_queue_pending_count());

    uint8_t mac[APP_MAC_LEN];
    espnow_link_get_sta_mac(mac);
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "sta_mac", mac_str);

    chat_peer_t peer = {};
    const bool have_peer = retry_manager_get_active_peer(&peer);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddBoolToObject(p, "discovered", have_peer);
    cJSON_AddBoolToObject(p, "online", have_peer && peer.online);
    cJSON_AddNumberToObject(p, "node_id", have_peer ? peer.node_id : 0);
    cJSON_AddStringToObject(p, "name", have_peer ? peer.name : "");
    cJSON_AddNumberToObject(p, "rssi", have_peer ? peer.rssi : 0);
    cJSON_AddItemToObject(root, "peer", p);

    messaging_stats_t st = {};
    retry_manager_get_stats(&st);
    cJSON *st_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(st_obj, "tx_sent", st.tx_sent);
    cJSON_AddNumberToObject(st_obj, "tx_acked_first", st.tx_acked_first);
    cJSON_AddNumberToObject(st_obj, "tx_retried_ack", st.tx_retried_ack);
    cJSON_AddNumberToObject(st_obj, "tx_retransmissions", st.tx_retransmissions);
    cJSON_AddNumberToObject(st_obj, "tx_failed", st.tx_failed);
    cJSON_AddNumberToObject(st_obj, "rx_received", st.rx_received);
    cJSON_AddNumberToObject(st_obj, "rx_duplicates", st.rx_duplicates);
    cJSON_AddNumberToObject(st_obj, "sim_loss_pct", st.sim_loss_pct);
    cJSON_AddItemToObject(root, "stats", st_obj);

    test_summary_t ts = {};
    retry_manager_get_test_summary(&ts);
    cJSON_AddItemToObject(root, "last_test", test_summary_to_json(&ts, "summary"));

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *ws_build_history_json(void)
{
    chat_message_t *msgs = (chat_message_t *)calloc(MSG_HISTORY_CAPACITY, sizeof(chat_message_t));
    if (!msgs) return NULL;
    const size_t n = message_queue_get_history(msgs, MSG_HISTORY_CAPACITY);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; ++i) {
        cJSON_AddItemToArray(arr, message_to_json(&msgs[i]));
    }
    free(msgs);
    cJSON_AddItemToObject(root, "messages", arr);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void ws_handler_init(httpd_handle_t server)
{
    s_server = server;
    if (!s_ws_mutex) {
        s_ws_mutex = xSemaphoreCreateMutex();
    }
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        s_clients[i] = -1;
    }
}

void ws_handler_on_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    remove_client(sockfd);
    close(sockfd);
}

void ws_handler_broadcast_msg_event(msg_event_type_t event, const chat_message_t *msg, void *ctx)
{
    (void)ctx;
    if (event == MSG_EVENT_HISTORY_CLEARED) {
        broadcast_json("{\"type\":\"history_cleared\"}");
        return;
    }
    if (!msg) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",
                            (event == MSG_EVENT_ADDED) ? "message" : "status_update");
    cJSON_AddItemToObject(root, "message", message_to_json(msg));
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        broadcast_json(json);
        free(json);
    }
    if (event == MSG_EVENT_STATUS_CHANGED) {
        ws_handler_broadcast_status();
    }
}

void ws_handler_broadcast_peer_event(const chat_peer_t *peer, void *ctx)
{
    (void)peer;
    (void)ctx;
    ws_handler_broadcast_status();
}

void ws_handler_broadcast_test_event(const test_summary_t *summary, bool finished, void *ctx)
{
    (void)ctx;
    if (!summary) return;
    cJSON *obj = test_summary_to_json(summary, finished ? "test_result" : "test_progress");
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json) {
        broadcast_json(json);
        free(json);
    }
}

void ws_handler_broadcast_status(void)
{
    char *json = ws_build_status_json();
    if (json) {
        broadcast_json(json);
        free(json);
    }
}

static void handle_ws_command(int fd, const char *payload, size_t len)
{
    char *buf = (char *)malloc(len + 1);
    if (!buf) return;
    memcpy(buf, payload, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        if (strcmp(type->valuestring, "send_text") == 0) {
            const cJSON *txt = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(txt) && txt->valuestring) {
                const esp_err_t err = retry_manager_send_text(txt->valuestring, NULL);
                if (err != ESP_OK) {
                    cJSON *resp = cJSON_CreateObject();
                    cJSON_AddStringToObject(resp, "type", "error");
                    cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
                    char *out = cJSON_PrintUnformatted(resp);
                    cJSON_Delete(resp);
                    if (out) {
                        send_json_to_fd(fd, out);
                        free(out);
                    }
                }
            }
        } else if (strcmp(type->valuestring, "run_test") == 0) {
            const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(root, "count");
            const cJSON *rel = cJSON_GetObjectItemCaseSensitive(root, "reliable");
            const unsigned count = cJSON_IsNumber(cnt) ? (unsigned)cnt->valueint : 100u;
            const bool reliable = cJSON_IsBool(rel) ? cJSON_IsTrue(rel) : true;
            const esp_err_t err = retry_manager_start_test(count, reliable);
            if (err != ESP_OK) {
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddStringToObject(resp, "type", "error");
                cJSON_AddStringToObject(resp, "error",
                                        err == ESP_ERR_NOT_FOUND ? "No peer discovered yet" : esp_err_to_name(err));
                char *out = cJSON_PrintUnformatted(resp);
                cJSON_Delete(resp);
                if (out) {
                    send_json_to_fd(fd, out);
                    free(out);
                }
            }
        } else if (strcmp(type->valuestring, "set_loss") == 0) {
            const cJSON *pct = cJSON_GetObjectItemCaseSensitive(root, "percent");
            if (cJSON_IsNumber(pct)) {
                int p = pct->valueint;
                if (p < 0) p = 0;
                if (p > 100) p = 100;
                retry_manager_set_loss_percent((unsigned)p);
                ws_handler_broadcast_status();
            }
        } else if (strcmp(type->valuestring, "drop_next_ack") == 0) {
            retry_manager_drop_next_ack();
            send_json_to_fd(fd, "{\"type\":\"notice\",\"text\":\"Next incoming DATA ACK will be dropped once.\"}");
        } else if (strcmp(type->valuestring, "clear_history") == 0) {
            message_queue_clear_history();
        } else if (strcmp(type->valuestring, "get_status") == 0) {
            char *st = ws_build_status_json();
            if (st) {
                send_json_to_fd(fd, st);
                free(st);
            }
        }
    }
    cJSON_Delete(root);
}

esp_err_t ws_handler_endpoint(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        const int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
        char *st = ws_build_status_json();
        if (st) {
            send_json_to_fd(fd, st);
            free(st);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len > 0 && ws_pkt.len <= 1024) {
        uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK && ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            handle_ws_command(httpd_req_to_sockfd(req), (const char *)buf, ws_pkt.len);
        }
        free(buf);
    }
    return ret;
}
