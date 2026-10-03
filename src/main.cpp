#include "AppConfig.h"
#include "UserInterface.h"
#include <math.h>
#include <driver/rtc_io.h>

class MyServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc)
    {
        deviceConnected = true;
        pServer->updateConnParams(desc->conn_handle, 800, 800, 29, 6000);
        ui_post_event(UI_EVT_BLE_CHANGED);
    }

    void onDisconnect(NimBLEServer *pServer)
    {
        deviceConnected = false;
        // Do not advertise again if the user turned Bluetooth off from the device
        if (!bleUserDisabled)
        {
            NimBLEDevice::startAdvertising();
        }
        ui_post_event(UI_EVT_BLE_CHANGED);
    }
};

void init_config_peripherals()
{
    pinMode(EN_LDO, OUTPUT);
    pinMode(EPD_CONTROL, OUTPUT);

    pinMode(BMV_CS_PIN, OUTPUT);
    pinMode(MOSI_PIN, OUTPUT);
    pinMode(SCK_PIN, OUTPUT);
    pinMode(MISO_PIN, INPUT);

    digitalWrite(BMV_CS_PIN, HIGH);
    digitalWrite(EN_LDO, LOW);
    digitalWrite(EPD_CONTROL, HIGH);

    pinMode(BUTTON_1, INPUT_PULLUP);
    vTaskDelay(pdMS_TO_TICKS(200));
}

// Levels kept in deep sleep are latched with GPIO hold; release them once the
// pins have been set again in init_config_peripherals()
void release_off_state_holds()
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)EN_LDO);
    gpio_hold_dis((gpio_num_t)EPD_CONTROL);
    gpio_hold_dis((gpio_num_t)BMV_CS_PIN);
}

void wait_button1_release()
{
    while (digitalRead(BUTTON_1) == LOW)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // debounce
}

// "Everything off": BLE never started, sensor rail and screen unpowered,
// chip in deep sleep. Only button 1 wakes it, which reboots through setup().
[[noreturn]] void enter_off_state()
{
    // Sensor rail off, screen off. BMV080 CS low so it cannot back-power the
    // unpowered sensor through its input pin.
    digitalWrite(EN_LDO, LOW);
    digitalWrite(EPD_CONTROL, HIGH);
    pinMode(BMV_CS_PIN, OUTPUT);
    digitalWrite(BMV_CS_PIN, LOW);

    // Keep these levels during deep sleep
    gpio_hold_en((gpio_num_t)EN_LDO);
    gpio_hold_en((gpio_num_t)EPD_CONTROL);
    gpio_hold_en((gpio_num_t)BMV_CS_PIN);
    gpio_deep_sleep_hold_en();

    // A held button would wake the chip again immediately
    wait_button1_release();

    // Wake when button 1 pulls GPIO0 low
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_1, 0);
    rtc_gpio_pullup_en((gpio_num_t)BUTTON_1);
    rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_1);

    esp_deep_sleep_start();
}

void init_i2c_one()
{
    Wire1.begin(SDA_PIN1, SCL_PIN1, I2C_FREQ_1);

    maxlipo.begin(&Wire1);

    maxlipo.hibernate();

    rtc.begin(Wire1);

    stcc4.begin(Wire1, STCC4_I2C_ADDR_64);

    stcc4.exitSleepMode();

    stcc4.stopContinuousMeasurement();

    stcc4.enterSleepMode();
}

void init_ble_config()
{

    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_N12);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    pCharReadings = pService->createCharacteristic(
        UUID_READINGS,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    static const char label[] = "Readings v1 (20-byte packed record)";
    pCharReadings->createDescriptor("2901", NIMBLE_PROPERTY::READ)
        ->setValue((const uint8_t *)label, strlen(label));

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);

    pAdvertising->setMinInterval(160);
    pAdvertising->setMaxInterval(160);

    NimBLEDevice::startAdvertising();
}

void control_sensor_power(bool on_off)
{
    if (on_off)
    {
        digitalWrite(BMV_CS_PIN, HIGH);
        digitalWrite(EN_LDO, HIGH);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    else
    {
        digitalWrite(BMV_CS_PIN, HIGH);
        digitalWrite(EN_LDO, LOW);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void start_sensors(SparkFunBMV080SPI &bmv080, BME690_7semi &bme)
{

    pinMode(BMV_CS_PIN, OUTPUT);
    pinMode(MOSI_PIN, OUTPUT);
    pinMode(SCK_PIN, OUTPUT);
    pinMode(MISO_PIN, INPUT);

    digitalWrite(BMV_CS_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, BMV_CS_PIN);

    vTaskDelay(pdMS_TO_TICKS(100));

    bmv080.begin(BMV_CS_PIN, SPI);
    bmv080.open();
    bmv080.reset();
    bmv080.setMode(SF_BMV080_MODE_CONTINUOUS);

    Wire.begin(SDA_PIN0, SCL_PIN0, I2C_FREQ_0);
    bme.begin(Wire);
}

void read_stcc4()
{
    uint16_t status = 0;
    float stccTemp = 0.0f;
    float stccRh = 0.0f;

    stcc4.exitSleepMode();

    stcc4.measureSingleShot();

    stcc4.readMeasurement(ble_co2, stccTemp, stccRh, status);

    stcc4.enterSleepMode();
}

void read_max()
{
    maxlipo.wake();
    ble_batt = maxlipo.cellPercent();
    maxlipo.hibernate();
}

void read_rtc(){
    rtc.updateTime();
}

void read_air_and_bme(SparkFunBMV080SPI &bmv080, BME690_7semi &bme)
{
    const uint32_t startMs = millis();

    while (millis() - startMs < 2000)
    {
        bool bmvOk = bmv080.readSensor();
        bool bmeOk = bme.readSensorData();

        if (bmvOk)
        {
            ble_pm10 = bmv080.PM10();
            ble_pm25 = bmv080.PM25();
            ble_pm1 = bmv080.PM1();
        }
        else if (bmv080.isObstructed())
        {
            ble_pm10 = 0.0f;
            ble_pm25 = 0.0f;
            ble_pm1 = 0.0f;
        }

        if (bmeOk)
        {
            ble_temp = bme.getTemperature();
            ble_rh = bme.getHumidity();
            ble_press = bme.getPressure();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void read_sensors(SparkFunBMV080SPI &bmv080, BME690_7semi &bme)
{
    read_stcc4();
    read_air_and_bme(bmv080, bme);
    read_max();
    read_rtc();
}

// Scale, round and clamp a float into the packet field; NaN/inf becomes the
// "no value" marker. Valid values are clamped one below the marker so they
// can never be mistaken for it.
static uint16_t packU16(float value, float scale)
{
    if (isnan(value) || isinf(value))
        return NO_VALUE_U16;
    return (uint16_t)constrain(lroundf(value * scale), 0L, (long)NO_VALUE_U16 - 1);
}

static int16_t packI16(float value, float scale)
{
    if (isnan(value) || isinf(value))
        return NO_VALUE_I16;
    return (int16_t)constrain(lroundf(value * scale), (long)NO_VALUE_I16 + 1, (long)INT16_MAX);
}

static uint8_t packU8(float value)
{
    if (isnan(value) || isinf(value))
        return NO_VALUE_U8;
    return (uint8_t)constrain(lroundf(value), 0L, (long)NO_VALUE_U8 - 1);
}

// Calendar date/time -> seconds since 1970-01-01 (days_from_civil algorithm)
static uint32_t toEpoch(int32_t y, uint32_t m, uint32_t d, uint32_t hh, uint32_t mm, uint32_t ss)
{
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int32_t days = era * 146097 + (int32_t)doe - 719468;
    return (uint32_t)days * 86400UL + hh * 3600UL + mm * 60UL + ss;
}

// Sends one reading as a single 20-byte notification.
// Time comes from the RTC registers read by read_rtc().
void send_data_ble()
{
    reading_packet_t p;
    p.version = PACKET_VERSION;
    p.timestamp = toEpoch(rtc.getYear(), rtc.getMonth(), rtc.getDate(),
                          rtc.getHours(), rtc.getMinutes(), rtc.getSeconds());
    p.pm1 = packU16(ble_pm1, 1);
    p.pm25 = packU16(ble_pm25, 1);
    p.pm10 = packU16(ble_pm10, 1);
    p.co2 = packU16(ble_co2, 1);
    p.temp = packI16(ble_temp, 10);
    p.hum = packU16(ble_rh, 10);
    p.pres = packU16(ble_press, 10); // BME690 library reports hPa
    p.battery = packU8(min(ble_batt, 100.0f)); // Gauge can read slightly above 100 % when full

    pCharReadings->setValue((const uint8_t *)&p, sizeof p); // Also readable on demand
    pCharReadings->notify();
}

void stop_sensors()
{
    SPI.end();

    pinMode(MOSI_PIN, INPUT);
    pinMode(SCK_PIN, INPUT);
    pinMode(MISO_PIN, INPUT);
    pinMode(BMV_CS_PIN, INPUT);

    control_sensor_power(false);
}

void run_sensor_cycle()
{
    SparkFunBMV080SPI bmv080;
    BME690_7semi bme(0x76, BME690_7semi::MODE_I2C);

    esp_pm_lock_acquire(sensorNoSleepLock);
    esp_pm_lock_acquire(sensorApbLock);

    control_sensor_power(true);

    start_sensors(bmv080, bme);

    read_sensors(bmv080, bme);

    bmv080.close();

    stop_sensors();

    send_data_ble();

    esp_pm_lock_release(sensorApbLock);
    esp_pm_lock_release(sensorNoSleepLock);
}

void setup()
{
    // Button 1 is left in RTC mode after a deep sleep wake-up
    rtc_gpio_deinit((gpio_num_t)BUTTON_1);

    init_config_peripherals();
    release_off_state_holds();

    // Puts STCC4 and MAX17048 into their sleep modes
    init_i2c_one();

    // Power-on or reset: stay fully off until button 1 is pressed
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0)
    {
        enter_off_state();
    }

    // Woken by button 1. Wait for release so the UI does not see this
    // press as a second "button 1" action.
    wait_button1_release();

    init_ble_config();

    esp_pm_config_esp32s3_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 10,
        .light_sleep_enable = true};

    esp_pm_configure(&pm_config);

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sensor_no_sleep", &sensorNoSleepLock);
    esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "sensor_apb", &sensorApbLock);

    // Screen + buttons. Draws the pairing screen.
    ui_init();
}

void loop()
{
    static bool cycleDoneThisConnection = false;
    static uint32_t lastCycleMs = 0;

    // Button presses / BLE changes that arrived meanwhile + screen update
    ui_process_events();

    if (!deviceConnected)
    {
        cycleDoneThisConnection = false;
        // Light sleep until a button press or BLE connect wakes us
        ui_wait_for_event(SENSOR_PERIOD_MS);
        return;
    }

    // First cycle right after connecting, then every SENSOR_PERIOD_MS
    const uint32_t sinceLast = millis() - lastCycleMs;
    if (!cycleDoneThisConnection || sinceLast >= SENSOR_PERIOD_MS)
    {
        run_sensor_cycle();
        lastCycleMs = millis();
        cycleDoneThisConnection = true;
        return;
    }

    // Light sleep until the next cycle, or earlier if button 1 is pressed
    ui_wait_for_event(SENSOR_PERIOD_MS - sinceLast);
}
