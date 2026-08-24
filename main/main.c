#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "status_led.h"
#include "vigilant.h"
#include "broadband.h"

/* Set to 1 for transmitter, 0 for receiver. */
#define BB_TRANSMITTER 1

#define BB_CHANNEL       6
#define BB_MAX_PAYLOAD   512
#define BB_PROTOCOL_VER  1

static const char *TAG = "Main";

void app_main(void) {
    VigilantConfig VgConfig = {.unique_component_name = "Comms-Broadband",
                               .network_mode = NW_MODE_STA};
    ESP_ERROR_CHECK(vigilant_init(VgConfig));

    initBroadband();
}