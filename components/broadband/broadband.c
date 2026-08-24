#include "broadband.h"

#include "esp_log.h"

static const char *TAG = "broadband";

esp_err_t initBroadband(void) {
    ESP_LOGI(TAG, "Initializing broadband component");
    return ESP_OK;
}