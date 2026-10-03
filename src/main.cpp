#include "AppConfig.h"
#include "UserInterface.h"
#include <math.h>

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

    pCharPM1 = pService->createCharacteristic(
        UUID_PM1,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharPM25 = pService->createCharacteristic(
        UUID_PM25,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharPM10 = pService->createCharacteristic(
        UUID_PM10,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharCO2 = pService->createCharacteristic(
        UUID_CO2,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharTMP = pService->createCharacteristic(
        UUID_TEMP,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharHUM = pService->createCharacteristic(
        UUID_HUM,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharPRESS = pService->createCharacteristic(
        UUID_PRESS,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharBATT = pService->createCharacteristic(
        UUID_BATTERY,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

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

int32_t roundedInt32(float value)
{
    if (isnan(value) || isinf(value))
    {
        return 0;
    }

    return (int32_t)lroundf(value);
}

int32_t batteryPercentForBle(float batteryPercent)
{
    return roundedInt32(batteryPercent * 100.0f);
}

void setInt32Value(NimBLECharacteristic *characteristic, int32_t value)
{
    characteristic->setValue(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
}

void send_data_ble()
{
    setInt32Value(pCharPM1, roundedInt32(ble_pm1));
    pCharPM1->notify();

    vTaskDelay(pdMS_TO_TICKS(20));

    setInt32Value(pCharPM25, roundedInt32(ble_pm25));
    pCharPM25->notify();

    vTaskDelay(pdMS_TO_TICKS(20));

    setInt32Value(pCharPM10, roundedInt32(ble_pm10));
    pCharPM10->notify();

    vTaskDelay(pdMS_TO_TICKS(20));

    setInt32Value(pCharCO2, static_cast<int32_t>(ble_co2));
    pCharCO2->notify();

    vTaskDelay(pdMS_TO_TICKS(20));

    setInt32Value(pCharTMP, roundedInt32(ble_temp));
    pCharTMP->notify();

    vTaskDelay(pdMS_TO_TICKS(20));

    setInt32Value(pCharHUM, roundedInt32(ble_rh));
    pCharHUM->notify();

    vTaskDelay(pdMS_TO_TICKS(20));

    setInt32Value(pCharPRESS, roundedInt32(ble_press));
    pCharPRESS->notify();

    setInt32Value(pCharBATT, batteryPercentForBle(ble_batt));
    pCharBATT->notify();
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

    init_config_peripherals();

    init_i2c_one();

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
