// ============================================================================
// EE4552 WSN - Gateway / Sink Node Firmware (ESP32-WROOM-32 Dual-Core)
// Architecture:
//   - Core 0 (Wi-Fi/ESP-NOW callback): Validate CRC, send APP_ACK immediately,
//     extract RSSI, enqueue to FreeRTOS Queue (zero blocking)
//   - Core 1 (loop): Duplicate suppression (SeenCache[16]), Node Timeout
//     monitoring (ALERT,NODE_OFFLINE), output clean CSV over USB-UART 115200
// Serial CSV Protocol (for SV3 Dashboard):
//   DATA,<src_id>,<via_id>,<seq_num>,<hop_count>,<temp_c>,<humi_rh>,<rssi_dbm>,<rtt_ms>
//   ALERT,NODE_OFFLINE,<node_id>
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "wsn_protocol.h"

struct GwQueueItem {
    WsnPacket pkt;
    int8_t    rssi_dbm;
};

static QueueHandle_t gwQueue = nullptr;
static SeenCache     seenCache;

// Node timeout tracking (indices 1..3)
static uint32_t g_last_seen_ms[4]   = { 0, 0, 0, 0 };
static bool     g_node_seen_ever[4] = { false, false, false, false };
static bool     g_node_online[4]    = { false, false, false, false };
static uint32_t g_last_timeout_chk  = 0;

// Fallback RSSI tracker for ESP-IDF < 5.0 (Arduino-ESP32 2.x)
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
static volatile int8_t g_last_promisc_rssi = -127;

static void promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* ppkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    g_last_promisc_rssi = (int8_t)ppkt->rx_ctrl.rssi;
}
#endif

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

static void sendAppAck(const WsnPacket& rxPkt) {
    WsnPacket ack = {};
    ack.pkt_type     = PKT_TYPE_ACK;
    ack.src_id       = NODE_ID_GATEWAY;
    ack.dst_id       = rxPkt.src_id;      // Final target is original sender
    ack.via_id       = NODE_ID_GATEWAY;
    ack.next_hop_id  = rxPkt.via_id;      // Send back to whoever handed us the packet
    ack.seq_num      = rxPkt.seq_num;
    ack.hop_count    = 1;
    ack.ttl          = WSN_DEFAULT_TTL;
    ack.temp_x100    = 0;
    ack.humi_x100    = 0;
    ack.timestamp_ms = rxPkt.timestamp_ms; // Echo original timestamp for RTT
    ack.last_rtt_ms  = 0;
    signPacketCRC(&ack);

    const uint8_t* replyMac = getMacByNodeId(rxPkt.via_id);
    if (replyMac != nullptr) {
        esp_now_send(replyMac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    int8_t rssi = (info && info->rx_ctrl) ? (int8_t)info->rx_ctrl->rssi : -127;
#else
static void onDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    (void)mac_addr;
    int8_t rssi = g_last_promisc_rssi;
#endif
    if (len != sizeof(WsnPacket)) return;

    WsnPacket pkt;
    memcpy(&pkt, data, sizeof(WsnPacket));
    if (!verifyPacketCRC(&pkt)) return;

    // Only accept DATA packets destined for Gateway
    if (pkt.pkt_type != PKT_TYPE_DATA || pkt.dst_id != NODE_ID_GATEWAY) return;

    // Immediately bounce APP_ACK on Core 0 for minimal RTT
    sendAppAck(pkt);

    // Enqueue to Core 1 for deduplication & Serial CSV output
    if (gwQueue != nullptr) {
        GwQueueItem item;
        item.pkt      = pkt;
        item.rssi_dbm = rssi;
        xQueueSend(gwQueue, &item, 0);
    }
}

void setup() {
    Serial.begin(WSN_SERIAL_BAUD);
    delay(300);

    gwQueue = xQueueCreate(64, sizeof(GwQueueItem));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(WSN_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_max_tx_power(WSN_TX_POWER_NORMAL_QDBM);

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promiscuousRxCb);
#endif

    if (esp_now_init() != ESP_OK) {
        Serial.println("# ERR,ESPNOW_INIT_FAILED");
        while (true) { delay(1000); }
    }

#if ENABLE_ESPNOW_ENCRYPTION
    esp_now_set_pmk(WSN_PMK_KEY);
#endif

    esp_now_register_recv_cb(onDataRecv);

    // Register all 3 sensor nodes (N1 included for Auto-Reroute direct link)
    registerPeer(MAC_NODE1);
    registerPeer(MAC_NODE2);
    registerPeer(MAC_NODE3);

    Serial.println("# INFO,GATEWAY_READY,CH=1,PKT_SIZE=20");
}

void loop() {
    // 1. Drain queue on Core 1 and emit clean CSV rows
    GwQueueItem item;
    while (xQueueReceive(gwQueue, &item, 0) == pdTRUE) {
        const WsnPacket& p = item.pkt;

        // Duplicate suppression
        if (seenCache.contains(p.src_id, p.seq_num, p.pkt_type)) {
            continue;
        }
        seenCache.insert(p.src_id, p.seq_num, p.pkt_type);

        // Update node liveness
        if (p.src_id >= 1 && p.src_id <= 3) {
            g_last_seen_ms[p.src_id]   = millis();
            g_node_seen_ever[p.src_id] = true;
            g_node_online[p.src_id]    = true;
        }

        // Format: DATA,<src_id>,<via_id>,<seq_num>,<hop_count>,<temp_c>,<humi_rh>,<rssi_dbm>,<rtt_ms>
        float temp_c  = p.temp_x100 / 100.0f;
        float humi_rh = p.humi_x100 / 100.0f;
        Serial.printf("DATA,%u,%u,%u,%u,%.2f,%.2f,%d,%u\n",
                      p.src_id,
                      p.via_id,
                      p.seq_num,
                      p.hop_count,
                      temp_c,
                      humi_rh,
                      (int)item.rssi_dbm,
                      p.last_rtt_ms);
    }

    // 2. Monitor node timeouts every 500 ms
    uint32_t now = millis();
    if (now - g_last_timeout_chk >= 500) {
        g_last_timeout_chk = now;
        for (uint8_t id = 1; id <= 3; id++) {
            if (g_node_seen_ever[id] && g_node_online[id]) {
                if (now - g_last_seen_ms[id] > WSN_NODE_OFFLINE_TIMEOUT_MS) {
                    g_node_online[id] = false;
                    Serial.printf("ALERT,NODE_OFFLINE,%u\n", id);
                }
            }
        }
    }
}
