#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "status_led.h"
#include "vigilant.h"

/* Set to 1 for transmitter, 0 for receiver. */
#define BB_TRANSMITTER 1

#define BB_CHANNEL       6
#define BB_MAX_PAYLOAD   512
#define BB_PROTOCOL_VER  1

static const char *TAG = "broadband";

/*
 * Locally administered BSSID used to identify our protocol.
 * This is not an actual access point.
 */
static const uint8_t BB_BSSID[6] = {
    0x02, 0x42, 0x42, 0x42, 0x42, 0x42
};

static const uint8_t BROADCAST_MAC[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/*
 * Basic 24-byte non-QoS 802.11 data header.
 */
typedef struct __attribute__((packed)) {
    uint8_t frame_control[2];
    uint8_t duration[2];
    uint8_t destination[6];
    uint8_t source[6];
    uint8_t bssid[6];
    uint8_t sequence_control[2];
} ieee80211_header_t;

/*
 * Our protocol header, placed directly after the 802.11 header.
 */
typedef struct __attribute__((packed)) {
    uint8_t magic[4];          /* "BBRD" */
    uint8_t version;
    uint8_t message_type;
    uint16_t payload_length;
    uint32_t packet_sequence;
    uint64_t timestamp_us;
} bb_header_t;

typedef struct {
    int8_t rssi;
    uint8_t message_type;
    uint16_t payload_length;
    uint32_t packet_sequence;
    uint64_t sender_timestamp_us;
    uint8_t payload[BB_MAX_PAYLOAD];
} bb_rx_message_t;

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

static void initialize_wifi(void)
{
    // The following calls are already made in vigilant_init() and wifi_init_once():
    // ESP_ERROR_CHECK(esp_netif_init()); 
    // ESP_ERROR_CHECK(esp_event_loop_create_default()); // Collides with Vigilant's event loop in line 251

    // wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();

    // ESP_ERROR_CHECK(esp_wifi_init(&config));
    // ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // Already done in VE wifi_init_once()
    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

#if BB_TRANSMITTER
    /*
     * Configure the rate used by esp_wifi_80211_tx().
     *
     * Alternatives:
     * WIFI_PHY_RATE_1M_L       DSSS + DBPSK
     * WIFI_PHY_RATE_2M_L       DSSS + DQPSK
     * WIFI_PHY_RATE_6M         OFDM + BPSK
     * WIFI_PHY_RATE_12M        OFDM + QPSK
     * WIFI_PHY_RATE_24M        OFDM + 16-QAM
     * WIFI_PHY_RATE_48M        OFDM + 64-QAM
     * WIFI_PHY_RATE_MCS0_LGI   HT-OFDM + BPSK
     */
    ESP_ERROR_CHECK(esp_wifi_config_80211_tx_rate(
        WIFI_IF_STA,
        WIFI_PHY_RATE_6M
    ));
#endif

    //ESP_ERROR_CHECK(esp_wifi_start());
    //ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /*
     * Standalone mode only. If connected to an AP later, the AP determines
     * the channel and this call should be removed.
     */
    //ESP_ERROR_CHECK(esp_wifi_set_channel( // W (628) wifi:STA is scanning or connecting, or AP has connected with external STAs, cannot set channel
    //    BB_CHANNEL,
    //    WIFI_SECOND_CHAN_NONE
    //));
}

#if BB_TRANSMITTER

static uint8_t transmitter_mac[6];
static uint32_t next_packet_sequence;

static esp_err_t bb_send(
    uint8_t message_type,
    const void *payload,
    size_t payload_length
)
{
    if (payload == NULL && payload_length != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_length > BB_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[
        sizeof(ieee80211_header_t) +
        sizeof(bb_header_t) +
        BB_MAX_PAYLOAD
    ] = {0};

    ieee80211_header_t *wifi_header = (void *)frame;

    /*
     * Frame Control:
     * version=0, type=data, subtype=normal data
     * ToDS=0, FromDS=0, Retry=0, Protected=0
     */
    wifi_header->frame_control[0] = 0x08;
    wifi_header->frame_control[1] = 0x00;

    memcpy(wifi_header->destination, BROADCAST_MAC, 6);
    memcpy(wifi_header->source, transmitter_mac, 6);
    memcpy(wifi_header->bssid, BB_BSSID, 6);

    bb_header_t protocol_header = {
        .magic = {'B', 'B', 'R', 'D'},
        .version = BB_PROTOCOL_VER,
        .message_type = message_type,
        .payload_length = (uint16_t)payload_length,
        .packet_sequence = next_packet_sequence++,
        .timestamp_us = (uint64_t)esp_timer_get_time(),
    };

    size_t offset = sizeof(ieee80211_header_t);

    memcpy(frame + offset, &protocol_header, sizeof(protocol_header));
    offset += sizeof(protocol_header);

    if (payload_length > 0) {
        memcpy(frame + offset, payload, payload_length);
        offset += payload_length;
    }

    /*
     * true lets the Wi-Fi driver generate the 802.11 sequence number.
     * Do not append an FCS; the hardware generates it.
     */
    return esp_wifi_80211_tx(
        WIFI_IF_STA,
        frame,
        offset,
        true
    );
}

static void transmitter_task(void *argument)
{
    uint32_t counter = 0;

    while (true) {
        char payload[128];

        int length = snprintf(
            payload,
            sizeof(payload),
            "Broadband packet %" PRIu32,
            counter++
        );

        esp_err_t err = bb_send(1, payload, (size_t)length);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sent: %s", payload);
        } else {
            ESP_LOGW(TAG, "Transmission failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#else

static QueueHandle_t receiver_queue;

static void promiscuous_rx_callback(
    void *buffer,
    wifi_promiscuous_pkt_type_t packet_type
)
{
    if (packet_type != WIFI_PKT_DATA) {
        return;
    }

    const wifi_promiscuous_pkt_t *received = buffer;
    const uint8_t *frame = received->payload;

    /*
     * sig_len includes the four-byte Wi-Fi FCS.
     */
    size_t frame_length = received->rx_ctrl.sig_len;

    const size_t minimum_length =
        sizeof(ieee80211_header_t) +
        sizeof(bb_header_t) +
        4; /* FCS */

    if (frame_length < minimum_length) {
        return;
    }

    ieee80211_header_t wifi_header;
    memcpy(&wifi_header, frame, sizeof(wifi_header));

    if (wifi_header.frame_control[0] != 0x08 ||
        wifi_header.frame_control[1] != 0x00) {
        return;
    }

    if (memcmp(wifi_header.destination, BROADCAST_MAC, 6) != 0) {
        return;
    }

    if (memcmp(wifi_header.bssid, BB_BSSID, 6) != 0) {
        return;
    }

    bb_header_t protocol_header;

    memcpy(
        &protocol_header,
        frame + sizeof(ieee80211_header_t),
        sizeof(protocol_header)
    );

    if (memcmp(protocol_header.magic, "BBRD", 4) != 0) {
        return;
    }

    if (protocol_header.version != BB_PROTOCOL_VER) {
        return;
    }

    size_t available_payload =
        frame_length -
        sizeof(ieee80211_header_t) -
        sizeof(bb_header_t) -
        4; /* FCS */

    if (protocol_header.payload_length > available_payload ||
        protocol_header.payload_length > BB_MAX_PAYLOAD) {
        return;
    }

    bb_rx_message_t message = {
        .rssi = received->rx_ctrl.rssi,
        .message_type = protocol_header.message_type,
        .payload_length = protocol_header.payload_length,
        .packet_sequence = protocol_header.packet_sequence,
        .sender_timestamp_us = protocol_header.timestamp_us,
    };

    memcpy(
        message.payload,
        frame +
            sizeof(ieee80211_header_t) +
            sizeof(bb_header_t),
        message.payload_length
    );

    /*
     * Do not block or log heavily inside the Wi-Fi callback.
     */
    (void)xQueueSend(receiver_queue, &message, 0);
}

static void receiver_task(void *argument)
{
    bb_rx_message_t message;

    while (true) {
        if (xQueueReceive(receiver_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(
            TAG,
            "RX seq=%" PRIu32
            " type=%u rssi=%d length=%u payload=\"%.*s\"",
            message.packet_sequence,
            message.message_type,
            message.rssi,
            message.payload_length,
            message.payload_length,
            (const char *)message.payload
        );
    }
}

static void initialize_receiver(void)
{
    receiver_queue = xQueueCreate(8, sizeof(bb_rx_message_t));
    assert(receiver_queue != NULL);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA
    };

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(
        esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_callback)
    );
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    xTaskCreate(
        receiver_task,
        "bb_receiver",
        4096,
        NULL,
        5,
        NULL
    );
}

#endif


void app_main(void) {
    VigilantConfig VgConfig = {.unique_component_name = "Comms-Broadband",
                               .network_mode = NW_MODE_STA};
    ESP_ERROR_CHECK(vigilant_init(VgConfig));
    ESP_LOGI(TAG, "Broadband component initialized");
    initialize_nvs();
    ESP_LOGI(TAG, "NVS initialized");
    initialize_wifi();
    ESP_LOGI(TAG, "Wi-Fi initialized");

#if BB_TRANSMITTER
    ESP_ERROR_CHECK(
        esp_wifi_get_mac(WIFI_IF_STA, transmitter_mac)
    );

    ESP_LOGI(
        TAG,
        "Transmitter started on channel %d, MAC %02x:%02x:%02x:%02x:%02x:%02x",
        BB_CHANNEL,
        transmitter_mac[0],
        transmitter_mac[1],
        transmitter_mac[2],
        transmitter_mac[3],
        transmitter_mac[4],
        transmitter_mac[5]
    );

    xTaskCreate(
        transmitter_task,
        "bb_transmitter",
        4096,
        NULL,
        5,
        NULL
    );
#else
    initialize_receiver();
    ESP_LOGI(TAG, "Receiver started on channel %d", BB_CHANNEL);
#endif
}