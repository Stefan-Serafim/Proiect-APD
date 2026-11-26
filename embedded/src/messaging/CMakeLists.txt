idf_component_register(
    SRCS "message_queue.cpp" "retry_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol radio spiffs nvs_flash esp_timer
)
