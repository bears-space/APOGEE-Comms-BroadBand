idf_component_register(
    SRCS "broadband.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        log
        nvs_flash
)
