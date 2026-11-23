// ============================================================================
// EE4552 WSN - Hardware Bring-Up Test (Blink Onboard LED + I2C Scan + SHT30)
// Supports both:
//   - test_hw_c3 (ESP32-C3 Super Mini): LED=GPIO8 (Active LOW),  I2C=GPIO4/5
//   - test_hw_gw (ESP32-WROOM-32)     : LED=GPIO2 (Active HIGH), I2C=GPIO21/22
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "sht30_sensor.h"

#ifndef ONBOARD_LED_PIN
#define ONBOARD_LED_PIN  8
#endif

#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW   1
#endif

#ifndef TEST_I2C_SDA
#define TEST_I2C_SDA     SHT30_I2C_SDA_PIN
#endif

#ifndef TEST_I2C_SCL
#define TEST_I2C_SCL     SHT30_I2C_SCL_PIN
#endif

static Sht30Sensor sensor;
static bool        led_state = false;
static bool        sht_found = false;

static void setLed(bool on) {
#if LED_ACTIVE_LOW
    digitalWrite(ONBOARD_LED_PIN, on ? LOW : HIGH);
#else
    digitalWrite(ONBOARD_LED_PIN, on ? HIGH : LOW);
#endif
}

static void scanI2C() {
    Serial.printf("\n--- Quet bus I2C (SDA=GPIO%d, SCL=GPIO%d) ---\n", TEST_I2C_SDA, TEST_I2C_SCL);
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  [OK] Phat hien thiet bi I2C tai dia chi: 0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0) {
        Serial.println("  [CANH BAO] Khong tim thay thiet bi I2C nao!");
        Serial.printf("  -> Kiem tra lai 4 day: VCC->3V3, GND->GND, SDA->GPIO%d, SCL->GPIO%d\n",
                      TEST_I2C_SDA, TEST_I2C_SCL);
    }
    Serial.println("-------------------------------------------");
}

void setup() {
    pinMode(ONBOARD_LED_PIN, OUTPUT);
    setLed(false);

    Serial.begin(WSN_SERIAL_BAUD);
    delay(1500);

    Serial.printf("\n=== TEST PHAN CUNG (LED=GPIO%d, I2C=GPIO%d/%d) ===\n",
                  ONBOARD_LED_PIN, TEST_I2C_SDA, TEST_I2C_SCL);

    Wire.begin(TEST_I2C_SDA, TEST_I2C_SCL);
    Wire.setClock(100000);
    scanI2C();

    sht_found = sensor.begin(TEST_I2C_SDA, TEST_I2C_SCL, SHT30_I2C_ADDR);
    if (sht_found) {
        Serial.println("[OK] Khoi tao cam bien SHT30 thanh cong!");
    } else {
        Serial.println("[INFO] Khong co SHT30 tai 0x44 (van nhap nhay LED & in terminal).");
    }
}

void loop() {
    // 1. Nhap nhay LED (dao trang thai moi 1 giay)
    led_state = !led_state;
    setLed(led_state);

    // 2. Doc cam bien SHT30 (neu co noi)
    int16_t  temp_x100 = 0;
    uint16_t humi_x100 = 0;
    if (sensor.read(temp_x100, humi_x100)) {
        Serial.printf("[LED=%s] Uptime=%lus | Nhiet do: %.2f C | Do am: %.2f %%RH\n",
                      led_state ? "ON " : "OFF",
                      millis() / 1000UL,
                      temp_x100 / 100.0f,
                      humi_x100 / 100.0f);
    } else {
        Serial.printf("[LED=%s] Uptime=%lus | Chua noi SHT30 (LED dang nhay OK)\n",
                      led_state ? "ON " : "OFF",
                      millis() / 1000UL);
    }

    delay(1000);
}
