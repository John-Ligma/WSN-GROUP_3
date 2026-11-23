#pragma once

#include <stdint.h>

// ============================================================================
// EE4552 WSN Cluster-Tree 2-Hop - Shared Configuration
// ============================================================================

// --- Node IDs ---
#define NODE_ID_GATEWAY             0x00
#define NODE_ID_LEAF1               0x01
#define NODE_ID_RELAY               0x02
#define NODE_ID_LEAF3               0x03

// --- RF & ESP-NOW Settings ---
#define WSN_WIFI_CHANNEL            1       // Fixed 2.4 GHz channel across all 4 boards
#if defined(CONFIG_IDF_TARGET_ESP32C3)
// ESP32-C3 Super Mini ceramic antenna suffers from RF self-interference at max +19.5 dBm;
// 34 (+8.5 dBm) is the proven stable sweet spot for ESP32-C3 Super Mini boards.
#define WSN_TX_POWER_NORMAL_QDBM    34      // +8.5 dBm (clean RF on ESP32-C3 Super Mini)
#define WSN_TX_POWER_MAX_QDBM       44      // +11.0 dBm for Auto-Reroute
#else
#define WSN_TX_POWER_NORMAL_QDBM    78      // +19.5 dBm on Gateway ESP32-WROOM-32U (external antenna)
#define WSN_TX_POWER_MAX_QDBM       78
#endif

// Set to 1 to enable AES-128 hardware encryption (Week 7+).
// Keep 0 during initial Week 5 bring-up if debugging raw frames, then set 1.
#define ENABLE_ESPNOW_ENCRYPTION    1

// 16-byte Primary Master Key (PMK) & Local Master Key (LMK) for AES-128
static const uint8_t WSN_PMK_KEY[16] = {
    'E','E','4','5','5','2','_','W','S','N','_','P','M','K','0','1'
};
static const uint8_t WSN_LMK_KEY[16] = {
    'E','E','4','5','5','2','_','W','S','N','_','L','M','K','0','1'
};

// --- Sampling & Transmission Periods ---
// Switch between 5000 ms (Test mode T1-T6) and 30000 ms (Low-power mode T7)
#define WSN_TX_PERIOD_5S_MS         5000UL
#define WSN_TX_PERIOD_30S_MS        30000UL
#define WSN_TX_PERIOD_MS            WSN_TX_PERIOD_5S_MS

// Anti-collision random jitter before TX (ms) - helps Test T4 (3 nodes)
#define WSN_JITTER_MIN_MS           5
#define WSN_JITTER_MAX_MS           30

// --- Reliability, RTT & Failure Detection ---
#define WSN_ACK_WAIT_MS             80UL    // Wait for APP_ACK (KPI 2-hop <= 60ms)
#define WSN_REROUTE_FAIL_THRESHOLD  3       // Switch to backup parent after 3 missed ACKs
#define WSN_REROUTE_PROBE_INTERVAL  5       // While rerouted, retry Relay every 5 packets
#define WSN_NODE_OFFLINE_TIMEOUT_MS (3UL * WSN_TX_PERIOD_MS + 2000UL) // 17s for T=5s

// --- Sensor I2C Pins (ESP32-C3 Super Mini, avoiding strapping pins GPIO8/9) ---
#define SHT30_I2C_SDA_PIN           4
#define SHT30_I2C_SCL_PIN           5
#define SHT30_I2C_ADDR              0x44

// --- Serial Baudrate ---
#define WSN_SERIAL_BAUD             115200

// --- Optional Debug & Test Modes ---
// Uncomment SIMULATE_SENSOR to send dummy T/RH values before SHT30 is wired
// #define SIMULATE_SENSOR

// Uncomment TEST_POWER_MODE for SV1 multimeter current measurement (Test T7):
//   1 = Continuous I2C Active (5s loop)
//   2 = Continuous ESP-NOW TX burst (5s loop)
//   3 = Continuous RX Idle (Always-On)
//   4 = Deep Sleep only (60s)
// #define TEST_POWER_MODE 1

// Enable verbose debug logs on Leaf/Relay Serial (Gateway always stays clean CSV)
#define ENABLE_NODE_DEBUG_LOG       1

// Set to 1 for RTC Deep Sleep on Leaf nodes; 0 to stay awake for debugging
#define ENABLE_DEEP_SLEEP           1
