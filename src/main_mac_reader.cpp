// ============================================================================
// EE4552 WSN - Hardware MAC Address Reader Utility
// Flash once to each board (mac_reader_c3 / mac_reader_gw) to copy its real
// Wi-Fi STA MAC address into include/wsn_protocol.h
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>

void setup() {
    Serial.begin(115200);
    delay(1000);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    uint8_t mac[6];
    WiFi.macAddress(mac);

    Serial.println();
    Serial.println("========================================");
    Serial.printf("STA MAC String : %s\n", WiFi.macAddress().c_str());
    Serial.printf("C Array Format : { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.println("========================================");
}

void loop() {
    delay(3000);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    Serial.printf("MAC: { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
