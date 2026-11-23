#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// ============================================================================
// Lightweight Self-Contained Sensirion SHT30 I2C Driver
// - No external library dependencies
// - Single-shot high repeatability (~15 ms conversion)
// - Built-in Sensirion CRC-8 (poly 0x31, init 0xFF) verification
// ============================================================================

class Sht30Sensor {
public:
    bool begin(int sda = SHT30_I2C_SDA_PIN, int scl = SHT30_I2C_SCL_PIN, uint8_t addr = SHT30_I2C_ADDR) {
        addr_ = addr;
#ifdef SIMULATE_SENSOR
        return true;
#else
        Wire.begin(sda, scl);
        Wire.setClock(100000);
        Wire.beginTransmission(addr_);
        return (Wire.endTransmission() == 0);
#endif
    }

    // Reads temperature (C * 100) and humidity (%RH * 100)
    bool read(int16_t& temp_x100, uint16_t& humi_x100) {
#ifdef SIMULATE_SENSOR
        // Deterministic simulated reading for bench testing without SHT30 wired
        float t = 26.50f + (float)(random(-50, 51)) / 100.0f;
        float h = 65.00f + (float)(random(-100, 101)) / 100.0f;
        temp_x100 = (int16_t)(t * 100.0f);
        humi_x100 = (uint16_t)(h * 100.0f);
        delay(15);
        return true;
#else
        // Single-shot measurement, high repeatability, clock stretching disabled (0x24 0x00)
        Wire.beginTransmission(addr_);
        Wire.write(0x24);
        Wire.write(0x00);
        if (Wire.endTransmission() != 0) {
            return false;
        }

        delay(16); // Datasheet max measurement duration = 15.5 ms

        uint8_t buf[6];
        uint8_t count = Wire.requestFrom(addr_, (uint8_t)6);
        if (count != 6) {
            return false;
        }
        for (uint8_t i = 0; i < 6; i++) {
            buf[i] = Wire.read();
        }

        if (shtCrc8(&buf[0], 2) != buf[2] || shtCrc8(&buf[3], 2) != buf[5]) {
            return false;
        }

        uint16_t rawT = ((uint16_t)buf[0] << 8) | buf[1];
        uint16_t rawH = ((uint16_t)buf[3] << 8) | buf[4];

        float tempC = -45.0f + 175.0f * ((float)rawT / 65535.0f);
        float humi  = 100.0f * ((float)rawH / 65535.0f);

        if (humi < 0.0f) humi = 0.0f;
        if (humi > 100.0f) humi = 100.0f;

        temp_x100 = (int16_t)(tempC * 100.0f);
        humi_x100 = (uint16_t)(humi * 100.0f);
        return true;
#endif
    }

private:
    uint8_t addr_ = SHT30_I2C_ADDR;

    static uint8_t shtCrc8(const uint8_t* data, size_t len) {
        uint8_t crc = 0xFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (uint8_t b = 0; b < 8; b++) {
                crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
            }
        }
        return crc;
    }
};
