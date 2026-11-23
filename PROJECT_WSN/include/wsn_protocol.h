#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "config.h"

// ============================================================================
// EE4552 WSN Binary Protocol (20 Bytes Fixed - Option B with piggyback RTT)
// ============================================================================

#define PKT_TYPE_DATA           0x01    // Uplink sensor telemetry
#define PKT_TYPE_ACK            0x02    // Downlink APP_ACK for valid RTT
#define WSN_DEFAULT_TTL         3       // Decremented at each relay; <= 1 dropped
#define WSN_SEEN_CACHE_SIZE     16      // Circular buffer for duplicate suppression
#define WSN_RTT_NONE            0       // First packet (no previous RTT yet)
#define WSN_RTT_TIMEOUT         65535   // Previous packet did not receive APP_ACK

// ----------------------------------------------------------------------------
// Hardware MAC Address Table (REPLACE WITH REAL MACs USING mac_reader_c3/gw!)
// ----------------------------------------------------------------------------
// Run `pio run -e mac_reader_c3 -t upload -t monitor` (or mac_reader_gw)
// on each board tomorrow and paste the printed MAC bytes below.
static const uint8_t MAC_GATEWAY[6] = { 0x48, 0xAF, 0xF3, 0x4F, 0x81, 0x3C }; // GW (0x00) - ESP32-WROOM-32U
static const uint8_t MAC_NODE1[6]   = { 0xB8, 0x1F, 0x3F, 0x0F, 0x5D, 0x94 }; // N1 (0x01) - ESP32-C3 Leaf 2-hop (chấm bút xóa)
static const uint8_t MAC_NODE2[6]   = { 0xB8, 0x1F, 0x3F, 0x0F, 0xF0, 0xD4 }; // N2 (0x02) - ESP32-C3 Relay (Always-On RX)
static const uint8_t MAC_NODE3[6]   = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x03 }; // N3 (0x03) - placeholder

inline const uint8_t* getMacByNodeId(uint8_t node_id) {
    switch (node_id) {
        case NODE_ID_GATEWAY: return MAC_GATEWAY;
        case NODE_ID_LEAF1:   return MAC_NODE1;
        case NODE_ID_RELAY:   return MAC_NODE2;
        case NODE_ID_LEAF3:   return MAC_NODE3;
        default:              return nullptr;
    }
}

// ----------------------------------------------------------------------------
// Fixed 20-Byte Binary Packet Structure
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint8_t  pkt_type;      // Byte 0     : 0x01=DATA, 0x02=APP_ACK
    uint8_t  src_id;        // Byte 1     : Original source node ID (1, 2, 3)
    uint8_t  dst_id;        // Byte 2     : Final destination ID (0=GW for DATA)
    uint8_t  via_id;        // Byte 3     : Previous-hop transmitter ID
    uint8_t  next_hop_id;   // Byte 4     : Intended next-hop receiver ID
    uint16_t seq_num;       // Byte 5-6   : Monotonic sequence number (1..65535)
    uint8_t  hop_count;     // Byte 7     : Hops traversed (init 1; +1 at Relay)
    uint8_t  ttl;           // Byte 8     : Time-To-Live (init 3; -1 at Relay)
    int16_t  temp_x100;     // Byte 9-10  : Temperature * 100 (e.g. 2765 = 27.65 C)
    uint16_t humi_x100;     // Byte 11-12 : Humidity * 100 (e.g. 6850 = 68.50 %RH)
    uint32_t timestamp_ms;  // Byte 13-16 : Sender's millis() at TX (for RTT)
    uint16_t last_rtt_ms;   // Byte 17-18 : Sender's measured RTT of previous pkt
    uint8_t  crc8;          // Byte 19    : CRC-8 (poly 0x07) over bytes 0..18
} WsnPacket;

static_assert(sizeof(WsnPacket) == 20, "WsnPacket must be strictly 20 bytes!");

// ----------------------------------------------------------------------------
// CRC-8 (Polynomial 0x07, Init 0x00)
// ----------------------------------------------------------------------------
inline uint8_t calcCRC8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

inline void signPacketCRC(WsnPacket* pkt) {
    pkt->crc8 = calcCRC8(reinterpret_cast<const uint8_t*>(pkt), sizeof(WsnPacket) - 1);
}

inline bool verifyPacketCRC(const WsnPacket* pkt) {
    uint8_t expected = calcCRC8(reinterpret_cast<const uint8_t*>(pkt), sizeof(WsnPacket) - 1);
    return (pkt->crc8 == expected);
}

// ----------------------------------------------------------------------------
// Circular Seen Cache (16 entries) for Duplicate Suppression
// Keyed by {src_id, seq_num, pkt_type}
// ----------------------------------------------------------------------------
struct SeenEntry {
    uint8_t  src_id;
    uint16_t seq_num;
    uint8_t  pkt_type;
    bool     valid;
};

class SeenCache {
public:
    SeenCache() : head_(0) {
        memset(entries_, 0, sizeof(entries_));
    }

    bool contains(uint8_t src_id, uint16_t seq_num, uint8_t pkt_type) const {
        for (uint8_t i = 0; i < WSN_SEEN_CACHE_SIZE; i++) {
            if (entries_[i].valid &&
                entries_[i].src_id == src_id &&
                entries_[i].seq_num == seq_num &&
                entries_[i].pkt_type == pkt_type) {
                return true;
            }
        }
        return false;
    }

    void insert(uint8_t src_id, uint16_t seq_num, uint8_t pkt_type) {
        entries_[head_].src_id   = src_id;
        entries_[head_].seq_num  = seq_num;
        entries_[head_].pkt_type = pkt_type;
        entries_[head_].valid    = true;
        head_ = (uint8_t)((head_ + 1) % WSN_SEEN_CACHE_SIZE);
    }

private:
    SeenEntry entries_[WSN_SEEN_CACHE_SIZE];
    uint8_t   head_;
};
