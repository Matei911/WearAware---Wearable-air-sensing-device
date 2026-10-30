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
// NaN when the STCC4 has no valid reading
float ble_co2 = NAN;

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
//   1   4   timestamp uint32  -      0            seconds since 1970-01-01, RTC local time,
//                                                  rounded to the minute (always hh:mm:00)
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

// ==================== Measurement cycle ====================
//
// While a phone is connected the device measures once per RTC minute:
//
//   hh:mm:45   wake, STCC4 continuous + BMV080 burst
//   hh:mm:58   measuring ends, sensors off
//   hh:mm+1:00 notification, timestamped with that whole minute
//   then light sleep until the next :45
//
// Cycles run at a fixed rate locked to the RTC, so the time spent measuring
// never pushes the next cycle later.
constexpr uint32_t SENSOR_PERIOD_MS = 60000;
constexpr uint32_t MEASURE_WINDOW_MS = 15000;  // wake -> notification
constexpr uint32_t MEASURE_TEARDOWN_MS = 2000; // STCC4 stop (1.2 s) + rail off + notify
constexpr uint32_t MEASURE_POLL_MS = 100;      // window loop tick
// A cycle that cannot start within this time of its slot (e.g. the user was in
// the disconnect dialog) is skipped, so a reading never lands in the wrong minute
constexpr uint32_t CYCLE_MAX_LATE_MS = 20000;

// BMV080: measured in a burst each sensor cycle (laser rail on, measure,
// laser rail off). Every driver output that covers a full integration window
// is kept (about 3 per cycle) and the median is sent; partial-window outputs
// read too low and are not used.
//   BMV080_INTEGRATION_S : measurement window. Longer = less noise, more power.
//   BMV080_ALGORITHM     : FAST_RESPONSE is what Bosch's own duty-cycling
//                          (short on/off bursts) mode uses. HIGH_PRECISION is
//                          the continuous-mode default and expects the sensor
//                          to keep running. BALANCED is in between.
constexpr float BMV080_INTEGRATION_S = 10.0f;
constexpr uint8_t BMV080_ALGORITHM = E_BMV080_MEASUREMENT_ALGORITHM_FAST_RESPONSE;
constexpr uint8_t BMV080_MAX_OUTPUTS = 8; // full-window outputs kept per cycle

// STCC4 (CO2): an SHT45 on its own sensor bus provides RH/T compensation.
// It runs in continuous mode (one reading per second) for the whole window
// instead of a single shot: its internal filtering works per sample, so more
// samples per minute make it follow CO2 changes much faster. The first
// readings after waking are dropped, the rest averaged.
constexpr uint32_t STCC4_READ_INTERVAL_MS = 1000;
constexpr uint8_t CO2_SETTLE_READINGS = 2;

// BME690: read only every 5 minutes to limit self-heating; every sensor cycle
// in between sends the last temperature / humidity / pressure again
constexpr uint32_t BME690_PERIOD_MS = 5UL * 60UL * 1000UL;
constexpr uint8_t BME690_ADDR = 0x76;
constexpr uint8_t BME690_REG_CTRL_GAS_1 = 0x71; // run_gas bits enable the gas heater
constexpr uint8_t BME690_RUN_GAS_MSK = 0x30;

// Set to 1 (here or with -D WEARAWARE_DEBUG_LOG=1 in build_flags) to print
// every cycle's raw readings on the USB serial port, e.g. to tune
// CO2_SETTLE_READINGS on the bench
#ifndef WEARAWARE_DEBUG_LOG
#define WEARAWARE_DEBUG_LOG 0
#endif
#if WEARAWARE_DEBUG_LOG
#define DEBUG_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_LOG(...) \
    do                 \
    {                  \
    } while (0)
#endif

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
