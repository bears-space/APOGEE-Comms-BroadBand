idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES vigilant_engine
    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        nvs_flash
)
