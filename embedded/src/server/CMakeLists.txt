idf_component_register(
    SRCS "web_server.cpp" "ws_handler.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol radio messaging esp_http_server spiffs json
)
