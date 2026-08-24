idf_component_register(
    SRCS "broadband.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_wifi" "esp_netif" "esp_event" "esp_timer" "esp_log" "nvs_flash"
)
