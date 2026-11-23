// ============================================================================
// EE4552 WSN - Relay + Sensor Node Firmware (Node 2, Always-On RX)
// Hardware: ESP32-C3 Super Mini + SHT30 (I2C GPIO4/GPIO5) + 18650 Battery
// Roles:
//   1. Non-blocking periodic self-sensing (1-hop -> GW, measures own RTT)
//   2. 2-way Relay Forwarding for Node 1:
//      - Uplink DATA:    N1 -> N2 -> GW (hop_count++, ttl--, via_id=2)
//      - Downlink ACK:   GW -> N2 -> N1 (hop_count++, ttl--, via_id=2)
//   3. Duplicate suppression (SeenCache[16]) & Loop prevention (TTL)
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "wsn_protocol.h"
#include "sht30_sensor.h"

static Sht30Sensor   sensor;
static SeenCache     seenCache;
static QueueHandle_t rxQueue = nullptr;

// Self-sensing state
static uint16_t g_self_seq         = 0;
static uint16_t g_self_last_rtt_ms = WSN_RTT_NONE;
static uint32_t g_last_sense_ms    = 0;
static bool     g_waiting_self_ack = false;
static uint32_t g_self_tx_time_ms  = 0;

static void registerPeer(const uint8_t* mac) {
    if (!mac) return;
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = WSN_WIFI_CHANNEL;
#if ENABLE_ESPNOW_ENCRYPTION
    peerInfo.encrypt = true;
    memcpy(peerInfo.lmk, WSN_LMK_KEY, 16);
#else
    peerInfo.encrypt = false;
#endif
    esp_now_add_peer(&peerInfo);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    (void)info;
#else
static void onDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    (void)mac_addr;
#endif
    if (len != sizeof(WsnPacket)) return;
    WsnPacket pkt;
    memcpy(&pkt, data, sizeof(WsnPacket));
    if (!verifyPacketCRC(&pkt)) return;

    // Push valid packet to FreeRTOS queue for immediate processing in loop()
    if (rxQueue != nullptr) {
        xQueueSend(rxQueue, &pkt, 0);
    }
}

static void handleIncomingPacket(WsnPacket& pkt) {
    // Case A: APP_ACK addressed directly to Node 2 (for its own self-sensed DATA)
    if (pkt.pkt_type == PKT_TYPE_ACK && pkt.dst_id == NODE_ID_RELAY) {
        if (g_waiting_self_ack && pkt.seq_num == g_self_seq) {
            uint32_t rtt = millis() - pkt.timestamp_ms;
            g_self_last_rtt_ms = (rtt > 65534UL) ? 65534 : (uint16_t)rtt;
            g_waiting_self_ack = false;
#if ENABLE_NODE_DEBUG_LOG
            Serial.printf("[N2-SELF] ACK seq=%u RTT=%u ms\n", pkt.seq_num, g_self_last_rtt_ms);
#endif
        }
        return;
    }

    // Case B: Relay Forwarding (only packets whose next_hop_id == NODE_ID_RELAY)
    if (pkt.next_hop_id != NODE_ID_RELAY) {
        return;
    }

    // Loop prevention: drop if TTL <= 1
    if (pkt.ttl <= 1) {
#if ENABLE_NODE_DEBUG_LOG
        Serial.printf("[N2-DROP] TTL expired src=%u seq=%u\n", pkt.src_id, pkt.seq_num);
#endif
        return;
    }

    // Duplicate suppression: check {src_id, seq_num, pkt_type}
    if (seenCache.contains(pkt.src_id, pkt.seq_num, pkt.pkt_type)) {
#if ENABLE_NODE_DEBUG_LOG
        Serial.printf("[N2-DUP] Dropped src=%u seq=%u type=%u\n",
                      pkt.src_id, pkt.seq_num, pkt.pkt_type);
#endif
        return;
    }
    seenCache.insert(pkt.src_id, pkt.seq_num, pkt.pkt_type);

    // Determine next hop based on direction
    uint8_t fwd_next_hop = 0xFF;
    if (pkt.pkt_type == PKT_TYPE_DATA && pkt.dst_id == NODE_ID_GATEWAY) {
        fwd_next_hop = NODE_ID_GATEWAY;
    } else if (pkt.pkt_type == PKT_TYPE_ACK && pkt.dst_id == NODE_ID_LEAF1) {
        fwd_next_hop = NODE_ID_LEAF1;
    } else {
        return;
    }

    // Update forwarding headers & recalculate CRC-8
    pkt.via_id      = NODE_ID_RELAY;
    pkt.next_hop_id = fwd_next_hop;
    pkt.hop_count   = (uint8_t)(pkt.hop_count + 1);
    pkt.ttl         = (uint8_t)(pkt.ttl - 1);
    signPacketCRC(&pkt);

    const uint8_t* dstMac = getMacByNodeId(fwd_next_hop);
    if (dstMac != nullptr) {
        esp_now_send(dstMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
#if ENABLE_NODE_DEBUG_LOG
        Serial.printf("[N2-FWD] type=%u src=%u dst=%u seq=%u hop=%u ttl=%u -> next=%u\n",
                      pkt.pkt_type, pkt.src_id, pkt.dst_id,
                      pkt.seq_num, pkt.hop_count, pkt.ttl, fwd_next_hop);
#endif
    }
}

static void sendSelfSensorData() {
    int16_t  temp_x100 = 0;
    uint16_t humi_x100 = 0;
    if (!sensor.read(temp_x100, humi_x100)) {
        temp_x100 = -9999;
        humi_x100 = 0;
    }

    g_self_seq++;
    if (g_self_seq == 0) g_self_seq = 1;

    WsnPacket pkt = {};
    pkt.pkt_type     = PKT_TYPE_DATA;
    pkt.src_id       = NODE_ID_RELAY;
    pkt.dst_id       = NODE_ID_GATEWAY;
    pkt.via_id       = NODE_ID_RELAY;
    pkt.next_hop_id  = NODE_ID_GATEWAY;
    pkt.seq_num      = g_self_seq;
    pkt.hop_count    = 1;
    pkt.ttl          = WSN_DEFAULT_TTL;
    pkt.temp_x100    = temp_x100;
    pkt.humi_x100    = humi_x100;
    pkt.last_rtt_ms  = g_self_last_rtt_ms;
    pkt.timestamp_ms = millis();
    signPacketCRC(&pkt);

    g_waiting_self_ack = true;
    g_self_tx_time_ms  = pkt.timestamp_ms;

    esp_now_send(MAC_GATEWAY, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));

#if ENABLE_NODE_DEBUG_LOG
    Serial.printf("[N2-SELF] TX seq=%u T=%.2f H=%.2f last_rtt=%u\n",
                  pkt.seq_num, temp_x100 / 100.0f, humi_x100 / 100.0f, g_self_last_rtt_ms);
#endif
}

void setup() {
    Serial.begin(WSN_SERIAL_BAUD);
    delay(200);

    rxQueue = xQueueCreate(32, sizeof(WsnPacket));

    bool sht_ok = sensor.begin();
    Serial.printf("[N2-RELAY] Booting... SHT30=%s\n", sht_ok ? "OK" : "FAIL");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(WSN_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_max_tx_power(WSN_TX_POWER_NORMAL_QDBM);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[N2-RELAY] ESP-NOW init FAILED!");
        while (true) { delay(1000); }
    }

#if ENABLE_ESPNOW_ENCRYPTION
    esp_now_set_pmk(WSN_PMK_KEY);
#endif

    esp_now_register_recv_cb(onDataRecv);

    // Register Gateway (uplink) and Node 1 (downlink ACK)
    registerPeer(MAC_GATEWAY);
    registerPeer(MAC_NODE1);

    g_last_sense_ms = millis();
    Serial.println("[N2-RELAY] Always-On RX Ready.");
}

void loop() {
    // 1. Highest priority: drain incoming packets & forward immediately
    WsnPacket rxPkt;
    while (xQueueReceive(rxQueue, &rxPkt, 0) == pdTRUE) {
        handleIncomingPacket(rxPkt);
    }

    // 2. Check self-ACK timeout without blocking
    uint32_t now = millis();
    if (g_waiting_self_ack && (now - g_self_tx_time_ms >= WSN_ACK_WAIT_MS)) {
        g_waiting_self_ack = false;
        g_self_last_rtt_ms = WSN_RTT_TIMEOUT;
#if ENABLE_NODE_DEBUG_LOG
        Serial.printf("[N2-SELF] ACK TIMEOUT seq=%u\n", g_self_seq);
#endif
    }

    // 3. Non-blocking periodic self-sensing
    if (now - g_last_sense_ms >= WSN_TX_PERIOD_MS) {
        g_last_sense_ms = now;
        sendSelfSensorData();
    }
}
