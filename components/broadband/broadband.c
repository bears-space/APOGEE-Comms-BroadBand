#include "broadband.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_timer.h"

static const char *TAG = "broadband";

#ifdef CONFIG_BB_COMM_TYPE_TX
    #define BB_TRANSMITTER 1
#else
    #define BB_TRANSMITTER 0
#endif

#if BB_TRANSMITTER // tx mode

static uint8_t transmitter_mac[6];
static uint32_t next_packet_sequence;

uint8_t broadcastMac[6];
uint8_t bssid[6];

static bool parseConfigMacs(void)
{
    unsigned int mac[6];

    int parsed = sscanf(
        CONFIG_BB_BROADCAST_MAC,
        "%2x:%2x:%2x:%2x:%2x:%2x",
        &mac[0], &mac[1], &mac[2],
        &mac[3], &mac[4], &mac[5]
    );

    if (parsed != 6) {
        // Invalid Kconfig MAC address
        return false;
    }

    for (int i = 0; i < 6; i++) {
        broadcastMac[i] = (uint8_t)mac[i];
    }

    parsed = sscanf(
        CONFIG_BB_BSSID,
        "%2x:%2x:%2x:%2x:%2x:%2x",
        &mac[0], &mac[1], &mac[2],
        &mac[3], &mac[4], &mac[5]
    );

    if (parsed != 6) {
        // Invalid Kconfig MAC address
        return false;
    }

    for (int i = 0; i < 6; i++) {
        bssid[i] = (uint8_t)mac[i];
    }

    return true;
}

static esp_err_t bb_send(
    uint8_t message_type,
    const void *payload,
    size_t payload_length
)
{
    if (payload == NULL && payload_length != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_length > CONFIG_BB_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[
        sizeof(ieee80211Header_t) +
        sizeof(BBHeader_t) +
        CONFIG_BB_MAX_PAYLOAD
    ] = {0};

    ieee80211Header_t *wifi_header = (void *)frame;

    /*
     * Frame Control:
     * version=0, type=data, subtype=normal data
     * ToDS=0, FromDS=0, Retry=0, Protected=0
     */
    wifi_header->frame_control[0] = 0x08;
    wifi_header->frame_control[1] = 0x00;

    memcpy(wifi_header->destination, broadcastMac, sizeof(broadcastMac));
    memcpy(wifi_header->source, transmitter_mac, sizeof(transmitter_mac));
    memcpy(wifi_header->bssid, bssid, sizeof(bssid));

    BBHeader_t protocolHeader = {
        .magic = {'B', 'B', 'R', 'D'},
        .version = CONFIG_BB_PROTOCOL_VER,
        .message_type = message_type,
        .payload_length = (uint16_t)payload_length,
        .packet_sequence = next_packet_sequence++,
        .timestamp_us = (uint64_t)esp_timer_get_time(),
    };

    size_t offset = sizeof(ieee80211Header_t);

    memcpy(frame + offset, &protocolHeader, sizeof(protocolHeader));
    offset += sizeof(protocolHeader);

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

#else // rx mode

static QueueHandle_t receiverQueue;

static void promiscuousRXCallback(
    void *buffer,
    wifi_promiscuous_pkt_type_t packetType
)
{
    /*
        In ESP-IDF, Wi-Fi promiscuous mode (often called sniffer mode) allows the ESP32 chip to capture all 802.11 wireless packets in the air on a specific channel, even if those data packets are not meant for its own MAC address. 

        this description was written by ai
    */


    if (packetType != WIFI_PKT_DATA) {
        return;
    }

    const wifi_promiscuous_pkt_t *received = buffer;
    const uint8_t *frame = received->payload;

    size_t frame_length = received->rx_ctrl.sig_len; // this includes the wifi frame check sequence (FCS) at the end

    const size_t minimum_length =
        sizeof(ieee80211Header_t) +
        sizeof(BBHeader_t) +
        4; // this is the frame check sequence (FCS) at the end of the frame

    if (frame_length < minimum_length) {
        return;
    }

    ieee80211Header_t wifi_header;
    memcpy(&wifi_header, frame, sizeof(wifi_header));

    if (wifi_header.frame_control[0] != 0x08 ||
        wifi_header.frame_control[1] != 0x00) {
        return;
    }

    if (memcmp(wifi_header.destination, broadcastMac, 6) != 0) { // set our mac
        return;
    }

    if (memcmp(wifi_header.bssid, bssid, 6) != 0) { // set the Basic Service Set Identifier
        return;
    }

    BBHeader_t protocolHeader;

    memcpy(
        &protocolHeader,
        frame + sizeof(ieee80211Header_t),
        sizeof(protocolHeader)
    );

    if (memcmp(protocolHeader.magic, "BBRD", 4) != 0) {
        return;
    }

    if (protocolHeader.version != CONFIG_BB_PROTOCOL_VER) {
        return;
    }

    size_t availablePayload =
        frame_length -
        sizeof(ieee80211Header_t) -
        sizeof(BBHeader_t) -
        4; // frame check sequence (FCS) at the end of the frame

    if (protocolHeader.payload_length > availablePayload ||
        protocolHeader.payload_length > CONFIG_BB_MAX_PAYLOAD) {
        return;
    }

    BBRXMessage_t message = {
        .rssi = received->rx_ctrl.rssi,
        .message_type = protocolHeader.message_type,
        .payload_length = protocolHeader.payload_length,
        .packet_sequence = protocolHeader.packet_sequence,
        .sender_timestamp_us = protocolHeader.timestamp_us,
    };

    memcpy(
        message.payload,
        frame +
            sizeof(ieee80211Header_t) +
            sizeof(BBHeader_t),
        message.payload_length
    );

    (void)xQueueSend(receiverQueue, &message, 0); // do not block the promiscuous callback, so we use a queue to send the message to the receiver task
}

static void receiverTask(void *argument)
{
    BBRXMessage_t message;

    while (true) {
        if (xQueueReceive(receiverQueue, &message, portMAX_DELAY) != pdTRUE) {
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

static void initializeReceiver(void)
{
    receiverQueue = xQueueCreate(8, sizeof(BBRXMessage_t));
    assert(receiverQueue != NULL);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA
    };

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(
        esp_wifi_set_promiscuous_rx_cb(promiscuousRXCallback)
    );
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    xTaskCreate(
        receiverTask,
        "bb_receiver",
        4096,
        NULL,
        5,
        NULL
    );
}
#endif

esp_err_t initBroadband(void) {
    ESP_LOGI(TAG, "Initializing broadband component");
    parseConfigMacs(); 
    ESP_LOGI(TAG, "Initializing wifi");
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
    ESP_LOGI(TAG, "Wi-Fi initialized");


    #if BB_TRANSMITTER
        ESP_ERROR_CHECK(
            esp_wifi_get_mac(WIFI_IF_STA, transmitter_mac)
        );

        ESP_LOGI(
            TAG,
            "Transmitter started on channel %d, MAC %02x:%02x:%02x:%02x:%02x:%02x",
            CONFIG_BB_CHANNEL,
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
        initializeReceiver();
        ESP_LOGI(TAG, "Receiver started on channel %d", CONFIG_BB_CHANNEL);
    #endif

    return ESP_OK;
}