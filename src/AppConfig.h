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

NimBLECharacteristic *pCharPM1 = nullptr;
NimBLECharacteristic *pCharPM25 = nullptr;
NimBLECharacteristic *pCharPM10 = nullptr;
NimBLECharacteristic *pCharCO2 = nullptr;
NimBLECharacteristic *pCharTMP = nullptr;
NimBLECharacteristic *pCharHUM = nullptr;
NimBLECharacteristic *pCharPRESS = nullptr;
NimBLECharacteristic *pCharBATT = nullptr;

float ble_pm1 = 0.0;
float ble_pm25 = 0.0;
float ble_pm10 = 0.0;
float ble_temp = 0.0;
float ble_rh = 0.0;
float ble_press = 0.0;
float ble_batt = 0.0;
int16_t ble_co2 = 0;

#define SERVICE_UUID "0000181a-0000-1000-8000-00805f9b34fc"
#define UUID_TEMP "00002A6E-0000-1000-8000-00805F9B34FB"
#define UUID_HUM "00002A6F-0000-1000-8000-00805F9B34FB"
#define UUID_PRESS "00002A6D-0000-1000-8000-00805F9B34FB"
#define UUID_CO2 "00002B8C-0000-1000-8000-00805F9B34FB"
#define UUID_PM1 "00002BD5-0000-1000-8000-00805F9B34FB"
#define UUID_PM25 "00002BD6-0000-1000-8000-00805F9B34FB"
#define UUID_PM10 "00002BD7-0000-1000-8000-00805F9B34FB"
#define UUID_BATTERY "00002A76-0000-1000-8000-00805F9B34FB"

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
