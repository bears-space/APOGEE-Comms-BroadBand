#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

// basic 24-byte non-QoS 802.11 data header.
typedef struct __attribute__((packed)) {
    uint8_t frame_control[2];
    uint8_t duration[2];
    uint8_t destination[6];
    uint8_t source[6];
    uint8_t bssid[6];
    uint8_t sequence_control[2];
} ieee80211Header_t;

// our protocol header
typedef struct __attribute__((packed)) {
    uint8_t magic[4];          // "BBRD"
    uint8_t version;
    uint8_t message_type;
    uint16_t payload_length;
    uint32_t packet_sequence;
    uint64_t timestamp_us;
} BBHeader_t;

// our protocol RX message structure, should be changed to the decided one in the future.
typedef struct {
    int8_t rssi;
    uint8_t message_type;
    uint16_t payload_length;
    uint32_t packet_sequence;
    uint64_t sender_timestamp_us;
    uint8_t payload[CONFIG_BB_MAX_PAYLOAD];
} BBRXMessage_t;


esp_err_t initBroadband(void);
