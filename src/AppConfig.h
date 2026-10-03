#pragma once
#include <Arduino.h>
#include "esp_pm.h"
#include <NimBLEDevice.h>
#include "SparkFun_BMV080_Arduino_Library.h"
#include <SensirionI2cStcc4.h>
#include <7semi_BME690.h>
#include "Adafruit_MAX1704X.h"
#include <RV-3028-C7.h>



// BLE definitions
NimBLEServer *pServer = nullptr;
bool deviceConnected = false;
bool systemStarted = false;
bool bmeI2cStarted = false;
bool bmeReady = false;

// Set when the user turns Bluetooth off from the device (button 1).
// While true the device does not advertise, so the phone cannot reconnect.
volatile bool bleUserDisabled = false;

// Name shown in the phone's Bluetooth scan list and on the pairing screen
#define BLE_DEVICE_NAME "ESP32_S3_LowPower"

// Locks for light sleep
esp_pm_lock_handle_t sensorNoSleepLock = nullptr;
esp_pm_lock_handle_t sensorApbLock = nullptr;

// One read/notify characteristic carrying a whole reading as a packed record
NimBLECharacteristic *pCharReadings = nullptr;

// NaN = no valid reading -> sent as "no value"
float ble_pm1 = NAN;
float ble_pm25 = NAN;
float ble_pm10 = NAN;
// NaN until the first BME690 reading -> sent as "no value"
float ble_temp = NAN;
float ble_rh = NAN;
float ble_press = NAN;
float ble_batt = 0.0;
int16_t ble_co2 = 0;

// BLE: one custom service with ONE read/notify characteristic that carries a
// whole reading as a packed 20-byte binary record (see reading_packet_t).
#define SERVICE_UUID "8d3b0000-5c2f-4c38-9a1e-6f1d2b7a0c01"
#define UUID_READINGS "8d3b0010-5c2f-4c38-9a1e-6f1d2b7a0c01"

// ==================== BLE packet format ====================
//
// Little-endian, packed, 20 bytes: fits in one notification even at the
// default MTU (23 -> 20 payload).
// Python: struct.unpack('<BIHHHHhHHB', data)
//
//  off size field     type    scale  "no value"
//   0   1   version   uint8   -      (always PACKET_VERSION)
//   1   4   timestamp uint32  -      0            seconds since 1970-01-01, RTC local time
//   5   2   pm1       uint16  1      0xFFFF       ug/m3
//   7   2   pm25      uint16  1      0xFFFF       ug/m3
//   9   2   pm10      uint16  1      0xFFFF       ug/m3
//  11   2   co2       uint16  1      0xFFFF       ppm
//  13   2   temp      int16   x10    0x8000       degC   (382 = 38.2 C)
//  15   2   hum       uint16  x10    0xFFFF       %RH    (220 = 22.0 %)
//  17   2   pres      uint16  x10    0xFFFF       hPa    (10126 = 1012.6 hPa)
//  19   1   battery   uint8   1      0xFF         %

static constexpr uint8_t PACKET_VERSION = 1;

struct __attribute__((packed)) reading_packet_t
{
    uint8_t version;
    uint32_t timestamp;
    uint16_t pm1, pm25, pm10, co2;
    int16_t temp;
    uint16_t hum, pres;
    uint8_t battery;
};
static_assert(sizeof(reading_packet_t) == 20, "BLE packet must stay 20 bytes");

static constexpr uint16_t NO_VALUE_U16 = 0xFFFF;
static constexpr int16_t NO_VALUE_I16 = INT16_MIN;
static constexpr uint8_t NO_VALUE_U8 = 0xFF;

// BMV Pins
constexpr uint8_t BMV_CS_PIN = 36;

// PMIC LDO Regulator
constexpr uint8_t EN_LDO = 1;

// EPD Screen Enable (active LOW: LOW = screen powered, HIGH = screen off)
constexpr uint8_t EPD_CONTROL = 37;
constexpr uint8_t EPD_POWER_ON = LOW;
constexpr uint8_t EPD_POWER_OFF = HIGH;

// EPD Screen pins (shares the SPI bus SCK/MOSI/MISO with the BMV080)
constexpr uint8_t EPD_CS = 5;
constexpr uint8_t EPD_DC = 4;
constexpr uint8_t EPD_RST = 3;
constexpr uint8_t EPD_BUSY = 2;

// SPI Pins
constexpr uint8_t MOSI_PIN = 11;
constexpr uint8_t SCK_PIN = 12;
constexpr uint8_t MISO_PIN = 13;

// Buttons (active LOW, internal pull-up)
// Only BUTTON_1 can wake the ESP32 from light sleep.
// BUTTON_2 / BUTTON_3 are only read while the UI is waiting for an answer.
constexpr uint8_t BUTTON_1 = 0;
constexpr uint8_t BUTTON_2 = 34;
constexpr uint8_t BUTTON_3 = 33;

// Sensor cycle period while a phone is connected
constexpr uint32_t SENSOR_PERIOD_MS = 60000;

// BMV080: measured in a burst each sensor cycle (laser rail on, measure,
// laser rail off). The value sent is the first driver output that covers a
// full integration window; partial-window outputs are not used.
//   BMV080_INTEGRATION_S : measurement window. Longer = less noise, more power.
//   BMV080_ALGORITHM     : FAST_RESPONSE is what Bosch's own duty-cycling
//                          (short on/off bursts) mode uses. HIGH_PRECISION is
//                          the continuous-mode default and expects the sensor
//                          to keep running. BALANCED is in between.
constexpr float BMV080_INTEGRATION_S = 10.0f;
constexpr uint8_t BMV080_ALGORITHM = E_BMV080_MEASUREMENT_ALGORITHM_FAST_RESPONSE;
constexpr uint32_t BMV080_MAX_MEASURE_MS = 15000; // give up without a full window

// BME690: read only every 5 minutes to limit self-heating; every sensor cycle
// in between sends the last temperature / humidity / pressure again
constexpr uint32_t BME690_PERIOD_MS = 5UL * 60UL * 1000UL;
constexpr uint8_t BME690_ADDR = 0x76;
constexpr uint8_t BME690_REG_CTRL_GAS_1 = 0x71; // run_gas bits enable the gas heater
constexpr uint8_t BME690_RUN_GAS_MSK = 0x30;

// STCC4, MAX, RTC CONFIG
SensirionI2cStcc4 stcc4;
Adafruit_MAX17048 maxlipo;
RV3028 rtc;

// I2C1 -> STCC4, MAX, RTC
static constexpr uint8_t SDA_PIN1 = 8;
static constexpr uint8_t SCL_PIN1 = 9;
static constexpr uint32_t I2C_FREQ_1 = 100000;

// I2C0 -> BME690
static constexpr uint8_t SDA_PIN0 = 17;
static constexpr uint8_t SCL_PIN0 = 14;
static constexpr uint32_t I2C_FREQ_0 = 100000;
