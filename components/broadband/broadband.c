#include <stdio.h>
#include "broadband.h"

static const char *TAG = "broadband";

esp_err_t initBroadband() {
    ESP_LOGI(TAG, "Initializing broadband component");
    return ESP_OK;
}