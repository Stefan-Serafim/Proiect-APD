#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "message_queue.h"
#include "retry_manager.h"
#include "ws_handler.h"

static const char *TAG = "web_server";
static httpd_handle_t s_httpd = NULL;

static const char *mime_type_for(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0)   return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(ext, ".csv") == 0)  return "text/csv; charset=utf-8";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    return "text/plain; charset=utf-8";
}

static esp_err_t send_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    httpd_resp_set_type(req, mime_type_for(fs_path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char chunk[1024];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirecting to ESP32 Chat...", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    char *json = ws_build_status_json();
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t api_history_handler(httpd_req_t *req)
{
    char *json = ws_build_history_json();
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static bool read_req_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return false;
    }
    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        const int r = httpd_req_recv(req, buf + offset, remaining);
        if (r <= 0) {
            return false;
        }
        offset += r;
        remaining -= r;
    }
    buf[offset] = '\0';
    return true;
}

static esp_err_t api_send_handler(httpd_req_t *req)
{
    char body[512];
    if (!read_req_body(req, body, sizeof(body))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(root, "text");
    uint32_t msg_id = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(txt) && txt->valuestring) {
        err = retry_manager_send_text(txt->valuestring, &msg_id);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (err == ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg_id\":%u}", (unsigned)msg_id);
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t api_test_handler(httpd_req_t *req)
{
    char body[128] = {};
    unsigned count = 100;
    bool reliable = true;
    if (read_req_body(req, body, sizeof(body))) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(root, "count");
            const cJSON *rel = cJSON_GetObjectItemCaseSensitive(root, "reliable");
            if (cJSON_IsNumber(cnt)) count = (unsigned)cnt->valueint;
            if (cJSON_IsBool(rel)) reliable = cJSON_IsTrue(rel);
            cJSON_Delete(root);
        }
    }
    const esp_err_t err = retry_manager_start_test(count, reliable);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t api_sim_handler(httpd_req_t *req)
{
    char body[128];
    if (!read_req_body(req, body, sizeof(body))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    const cJSON *loss = cJSON_GetObjectItemCaseSensitive(root, "loss_percent");
    if (cJSON_IsNumber(loss)) {
        int p = loss->valueint;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        retry_manager_set_loss_percent((unsigned)p);
    }
    const cJSON *drop = cJSON_GetObjectItemCaseSensitive(root, "drop_next_ack");
    if (cJSON_IsBool(drop) && cJSON_IsTrue(drop)) {
        retry_manager_drop_next_ack();
    }
    cJSON_Delete(root);
    ws_handler_broadcast_status();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_clear_history_handler(httpd_req_t *req)
{
    message_queue_clear_history();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_metrics_csv_handler(httpd_req_t *req)
{
    const size_t cap = 20480;
    char *csv = (char *)malloc(cap);
    if (!csv) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    const size_t n = retry_manager_export_csv(csv, cap);
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"esp32_chat_metrics.csv\"");
    httpd_resp_send(req, csv, (ssize_t)n);
    free(csv);
    return ESP_OK;
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char uri_path[128];
    const char *q = strchr(req->uri, '?');
    const size_t raw_len = q ? (size_t)(q - req->uri) : strlen(req->uri);
    const size_t copy_len = raw_len < sizeof(uri_path) - 1 ? raw_len : sizeof(uri_path) - 1;
    memcpy(uri_path, req->uri, copy_len);
    uri_path[copy_len] = '\0';

    if (strcmp(uri_path, "/") == 0) {
        strcpy(uri_path, "/index.html");
    }

    // Prevent directory traversal
    if (strstr(uri_path, "..") != NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    }

    char fs_path[160];
    snprintf(fs_path, sizeof(fs_path), "/www%s", uri_path);
    if (send_file(req, fs_path) == ESP_OK) {
        return ESP_OK;
    }

    // Captive portal / SPA fallback: redirect unknown hosts or serve index.html
    return captive_redirect_handler(req);
}

esp_err_t web_server_start(void)
{
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    const esp_err_t fs_err = esp_vfs_spiffs_register(&spiffs_conf);
    if (fs_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount /www SPIFFS (%s)", esp_err_to_name(fs_err));
        return fs_err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.close_fn = ws_handler_on_close;

    const esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server (%s)", esp_err_to_name(err));
        return err;
    }

    ws_handler_init(s_httpd);

    const struct {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
        bool is_ws;
    } routes[] = {
        { "/ws",                       HTTP_GET,  ws_handler_endpoint,      true  },
        { "/api/status",               HTTP_GET,  api_status_handler,       false },
        { "/api/history",              HTTP_GET,  api_history_handler,      false },
        { "/api/send",                 HTTP_POST, api_send_handler,         false },
        { "/api/test",                 HTTP_POST, api_test_handler,         false },
        { "/api/sim",                  HTTP_POST, api_sim_handler,          false },
        { "/api/history/clear",        HTTP_POST, api_clear_history_handler,false },
        { "/api/metrics.csv",          HTTP_GET,  api_metrics_csv_handler,  false },
        { "/generate_204",             HTTP_GET,  captive_redirect_handler, false },
        { "/gen_204",                  HTTP_GET,  captive_redirect_handler, false },
        { "/hotspot-detect.html",      HTTP_GET,  captive_redirect_handler, false },
        { "/library/test/success.html",HTTP_GET,  captive_redirect_handler, false },
        { "/ncsi.txt",                 HTTP_GET,  captive_redirect_handler, false },
        { "/connecttest.txt",          HTTP_GET,  captive_redirect_handler, false },
        { "/*",                        HTTP_GET,  static_get_handler,       false },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        httpd_uri_t u = {};
        u.uri = routes[i].uri;
        u.method = routes[i].method;
        u.handler = routes[i].handler;
        u.is_websocket = routes[i].is_ws;
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u));
    }

    ESP_LOGI(TAG, "HTTP + WebSocket server started on http://192.168.4.1/");
    return ESP_OK;
}
