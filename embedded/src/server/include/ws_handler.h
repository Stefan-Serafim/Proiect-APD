#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "message_queue.h"
#include "retry_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void ws_handler_init(httpd_handle_t server);
esp_err_t ws_handler_endpoint(httpd_req_t *req);
void ws_handler_on_close(httpd_handle_t hd, int sockfd);

void ws_handler_broadcast_msg_event(msg_event_type_t event, const chat_message_t *msg, void *ctx);
void ws_handler_broadcast_peer_event(const chat_peer_t *peer, void *ctx);
void ws_handler_broadcast_test_event(const test_summary_t *summary, bool finished, void *ctx);
void ws_handler_broadcast_status(void);

char *ws_build_status_json(void);
char *ws_build_history_json(void);

#ifdef __cplusplus
}
#endif
