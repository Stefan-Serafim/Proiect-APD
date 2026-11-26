idf_component_register(
    SRCS "espnow_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES protocol esp_wifi esp_event esp_netif lwip esp_timer
)
