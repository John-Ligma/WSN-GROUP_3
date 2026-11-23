// ============================================================================
// EE4552 WSN - Leaf Node Firmware (Node 1: 2-Hop + Auto-Reroute, Node 3: 1-Hop)
// Hardware: ESP32-C3 Super Mini + SHT30 (I2C GPIO4/GPIO5) + 18650 Battery
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include "config.h"
#include "wsn_protocol.h"
#include "sht30_sensor.h"

#ifndef NODE_ID
#define NODE_ID NODE_ID_LEAF3
#endif

#ifndef DEFAULT_NEXT_HOP_ID
#define DEFAULT_NEXT_HOP_ID NODE_ID_GATEWAY
#endif

#ifndef ENABLE_AUTO_REROUTE
#define ENABLE_AUTO_REROUTE 0
#endif

// Persist state across RTC Deep Sleep cycles
RTC_DATA_ATTR uint16_t rtc_seq_num       = 0;
RTC_DATA_ATTR uint16_t rtc_last_rtt_ms   = WSN_RTT_NONE;
RTC_DATA_ATTR uint8_t  rtc_fail_cnt      = 0;
RTC_DATA_ATTR bool     rtc_reroute_mode  = false;
RTC_DATA_ATTR uint8_t  rtc_reroute_pkts  = 0;

static Sht30Sensor sensor;

// Shared state between ESP-NOW RX callback and main flow
static volatile bool     g_ack_received   = false;
static volatile uint16_t g_ack_seq        = 0;
static volatile uint32_t g_ack_ts_ms      = 0;
static volatile uint32_t g_ack_rx_time_ms = 0;

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

    if (pkt.pkt_type == PKT_TYPE_ACK && pkt.dst_id == NODE_ID) {
        g_ack_rx_time_ms = millis();
        g_ack_seq        = pkt.seq_num;
        g_ack_ts_ms      = pkt.timestamp_ms;
        g_ack_received   = true;
    }
}

static bool initEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(WSN_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_max_tx_power(WSN_TX_POWER_NORMAL_QDBM);

    if (esp_now_init() != ESP_OK) {
        return false;
    }

#if ENABLE_ESPNOW_ENCRYPTION
    esp_now_set_pmk(WSN_PMK_KEY);
#endif

    esp_now_register_recv_cb(onDataRecv);

    // Register primary parent and (for Node 1) backup Gateway peer
    registerPeer(getMacByNodeId(DEFAULT_NEXT_HOP_ID));
#if ENABLE_AUTO_REROUTE
    if (DEFAULT_NEXT_HOP_ID != NODE_ID_GATEWAY) {
        registerPeer(MAC_GATEWAY);
    }
#endif
    return true;
}

#ifdef TEST_POWER_MODE
static void runPowerTestMode() {
    Serial.printf("[T7 POWER MODE %d] Node %d\n", TEST_POWER_MODE, NODE_ID);
    if (TEST_POWER_MODE == 1) {
        // Continuous I2C reading
        sensor.begin();
        int16_t t; uint16_t h;
        uint32_t start = millis();
        while (millis() - start < 5000) {
            sensor.read(t, h);
        }
    } else if (TEST_POWER_MODE == 2) {
        // Continuous ESP-NOW TX
        initEspNow();
        WsnPacket pkt = {};
        pkt.pkt_type = PKT_TYPE_DATA;
        pkt.src_id = NODE_ID;
        pkt.dst_id = NODE_ID_GATEWAY;
        pkt.via_id = NODE_ID;
        pkt.next_hop_id = DEFAULT_NEXT_HOP_ID;
        pkt.hop_count = 1;
        pkt.ttl = WSN_DEFAULT_TTL;
        signPacketCRC(&pkt);
        const uint8_t* dstMac = getMacByNodeId(DEFAULT_NEXT_HOP_ID);
        uint32_t start = millis();
        while (millis() - start < 5000) {
            esp_now_send(dstMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
            delay(5);
        }
    } else if (TEST_POWER_MODE == 3) {
        // Continuous RX idle
        initEspNow();
        delay(5000);
    } else if (TEST_POWER_MODE == 4) {
        // Deep sleep 60s
        esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
        esp_deep_sleep_start();
    }
}
#endif

#define LEAF_LED_PIN 8  // ESP32-C3 Super Mini onboard blue LED (Active LOW)

static void blinkLed(uint8_t times, uint16_t on_ms = 50, uint16_t off_ms = 50) {
    pinMode(LEAF_LED_PIN, OUTPUT);
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(LEAF_LED_PIN, LOW);
        delay(on_ms);
        digitalWrite(LEAF_LED_PIN, HIGH);
        if (i + 1 < times) delay(off_ms);
    }
}

static void runOneCycle() {
    uint32_t wake_start_ms = millis();

    // 1. Read SHT30 sensor (~16 ms)
    int16_t  temp_x100 = 0;
    uint16_t humi_x100 = 0;
    bool sensor_ok = sensor.begin() && sensor.read(temp_x100, humi_x100);
    if (!sensor_ok) {
        temp_x100 = -9999;
        humi_x100 = 0;
    }

    // 2. Init ESP-NOW on fixed channel
#if ENABLE_DEEP_SLEEP
    if (!initEspNow()) {
#if ENABLE_NODE_DEBUG_LOG
        Serial.println("[ERR] ESP-NOW init failed");
#endif
        esp_sleep_enable_timer_wakeup((uint64_t)WSN_TX_PERIOD_MS * 1000ULL);
        esp_deep_sleep_start();
    }
#endif

    // 3. Determine next hop (Normal vs Auto-Reroute for Node 1)
    uint8_t next_hop = DEFAULT_NEXT_HOP_ID;
#if ENABLE_AUTO_REROUTE
    bool probing_relay = false;
    if (rtc_reroute_mode) {
        rtc_reroute_pkts++;
        if (rtc_reroute_pkts >= WSN_REROUTE_PROBE_INTERVAL) {
            rtc_reroute_pkts = 0;
            next_hop = DEFAULT_NEXT_HOP_ID;
            probing_relay = true;
        } else {
            esp_wifi_set_max_tx_power(WSN_TX_POWER_MAX_QDBM);
            next_hop = NODE_ID_GATEWAY;
        }
    }
#endif

    // 4. Random jitter to prevent multi-node collision (Test T4)
    randomSeed(esp_random());
    delay(random(WSN_JITTER_MIN_MS, WSN_JITTER_MAX_MS + 1));

    // 5. Build 20-byte WsnPacket
    rtc_seq_num++;
    if (rtc_seq_num == 0) rtc_seq_num = 1;

    WsnPacket pkt = {};
    pkt.pkt_type     = PKT_TYPE_DATA;
    pkt.src_id       = NODE_ID;
    pkt.dst_id       = NODE_ID_GATEWAY;
    pkt.via_id       = NODE_ID;
    pkt.next_hop_id  = next_hop;
    pkt.seq_num      = rtc_seq_num;
    pkt.hop_count    = 1;
    pkt.ttl          = WSN_DEFAULT_TTL;
    pkt.temp_x100    = temp_x100;
    pkt.humi_x100    = humi_x100;
    pkt.last_rtt_ms  = rtc_last_rtt_ms;

    g_ack_received   = false;
    pkt.timestamp_ms = millis();
    signPacketCRC(&pkt);

    const uint8_t* dstMac = getMacByNodeId(next_hop);
    esp_err_t tx_res = esp_now_send(dstMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));

    // 6. Wait for APP_ACK up to WSN_ACK_WAIT_MS to measure valid RTT
    uint32_t wait_start = millis();
    while (!g_ack_received && (millis() - wait_start < WSN_ACK_WAIT_MS)) {
        delay(1);
    }

    if (g_ack_received && g_ack_seq == pkt.seq_num) {
        uint32_t rtt = g_ack_rx_time_ms - g_ack_ts_ms;
        rtc_last_rtt_ms = (rtt > 65534UL) ? 65534 : (uint16_t)rtt;
        rtc_fail_cnt = 0;
#if ENABLE_AUTO_REROUTE
        if (probing_relay) {
            rtc_reroute_mode = false;
            rtc_reroute_pkts = 0;
        }
#endif
        blinkLed(2, 40, 40); // 2 quick blinks = TX + ACK OK!
#if ENABLE_NODE_DEBUG_LOG
        if (Serial) {
            Serial.printf("[N%d] TX seq=%u -> hop=%u T=%.2f H=%.2f | ACK OK RTT=%u ms\n",
                          NODE_ID, pkt.seq_num, next_hop,
                          temp_x100 / 100.0f, humi_x100 / 100.0f, rtc_last_rtt_ms);
        }
#endif
    } else {
        rtc_last_rtt_ms = WSN_RTT_TIMEOUT;
        rtc_fail_cnt++;
#if ENABLE_AUTO_REROUTE
        if (!rtc_reroute_mode && rtc_fail_cnt >= WSN_REROUTE_FAIL_THRESHOLD) {
            rtc_reroute_mode = true;
            rtc_reroute_pkts = 0;
#if ENABLE_NODE_DEBUG_LOG
            if (Serial) {
                Serial.printf("[N%d] ALERT: Relay lost (%u fails) -> Auto-Reroute to GW!\n",
                              NODE_ID, rtc_fail_cnt);
            }
#endif
        }
#endif
        blinkLed(1, 100, 0); // 1 long blink = TX sent, ACK timeout
#if ENABLE_NODE_DEBUG_LOG
        if (Serial) {
            Serial.printf("[N%d] TX seq=%u -> hop=%u (err=%d) | ACK TIMEOUT (fail=%u)\n",
                          NODE_ID, pkt.seq_num, next_hop, (int)tx_res, rtc_fail_cnt);
        }
#endif
    }

    // NOTE: Do NOT call Serial.flush() on ESP32-C3 Native USB CDC when running on battery/external power!

    // 7. Compute remaining sleep/delay time
    uint32_t elapsed_ms = millis() - wake_start_ms;
    uint32_t sleep_ms   = (elapsed_ms < WSN_TX_PERIOD_MS) ? (WSN_TX_PERIOD_MS - elapsed_ms) : 100;

#if ENABLE_DEEP_SLEEP
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);
    esp_deep_sleep_start();
#else
    delay(sleep_ms);
#endif
}

void setup() {
#if ENABLE_NODE_DEBUG_LOG
    Serial.begin(WSN_SERIAL_BAUD);
#if !ENABLE_DEEP_SLEEP
    delay(1500);
    Serial.printf("\n[N%d] Booting (NO-SLEEP MODE)\n", NODE_ID);
#endif
#endif

#ifdef TEST_POWER_MODE
    runPowerTestMode();
    return;
#endif

#if !ENABLE_DEEP_SLEEP
    if (!initEspNow()) {
        Serial.println("[ERR] ESP-NOW init failed!");
        while (true) { delay(1000); }
    }
    Serial.printf("[N%d] ESP-NOW Ready (CH=%d, ENC=%d)\n",
                  NODE_ID, WSN_WIFI_CHANNEL, ENABLE_ESPNOW_ENCRYPTION);
#else
    runOneCycle();
#endif
}

void loop() {
#ifdef TEST_POWER_MODE
    runPowerTestMode();
#elif !ENABLE_DEEP_SLEEP
    runOneCycle();
#endif
}
