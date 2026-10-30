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

    // BMV080: the IO/digital supply (connector pins 4/6) is always on, only
    // the laser/analog rail (pins 11/13) is switched by EN_LDO. Its chip
    // select must stay HIGH whenever the sensor is not accessed, otherwise
    // the powered interface can be selected and react to screen SPI traffic.
    // Keep the pin driven HIGH during light sleep too (the sdkconfig
    // otherwise disconnects every GPIO while sleeping).
    gpio_sleep_sel_dis((gpio_num_t)BMV_CS_PIN);

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
    // Only button 1 may wake the chip: drop the light-sleep GPIO wake-up and
    // the button interrupt used while running
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_intr_disable((gpio_num_t)BUTTON_1);
    gpio_wakeup_disable((gpio_num_t)BUTTON_1);

    // Sensor rail off, screen off. BMV080 CS HIGH (deselected): its IO and
    // digital supply stays powered from the always-on 3V3 rail.
    digitalWrite(EN_LDO, LOW);
    digitalWrite(EPD_CONTROL, HIGH);
    pinMode(BMV_CS_PIN, OUTPUT);
    digitalWrite(BMV_CS_PIN, HIGH);

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

// Sets up the SPI bus and prepares the BMV080 (driver open + reset like the
// SparkFun example, then the measurement parameters). The measurement itself
// is only started in bmv080_start(), right before it is served.
// Returns false if the BMV080 could not be prepared.
bool start_sensors(SparkFunBMV080SPI &bmv080)
{

    pinMode(BMV_CS_PIN, OUTPUT);
    pinMode(MOSI_PIN, OUTPUT);
    pinMode(SCK_PIN, OUTPUT);
    pinMode(MISO_PIN, INPUT);

    digitalWrite(BMV_CS_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, BMV_CS_PIN);

    vTaskDelay(pdMS_TO_TICKS(100));

    if (!bmv080.begin(BMV_CS_PIN, SPI) || !bmv080.init())
    {
        return false;
    }

    // Parameters must be set before the measurement is started
    return bmv080.setIntegrationTime(BMV080_INTEGRATION_S) &&
           bmv080.setMeasurementAlgorithm(BMV080_ALGORITHM);
}

// ==================== STCC4 (CO2) ====================

// Continuous CO2 readings collected during one window
struct Co2Burst
{
    bool running = false;
    uint8_t readings = 0; // successful reads, settling ones included
    uint8_t summed = 0;   // reads after the first CO2_SETTLE_READINGS
    float sum = 0.0f;
    uint32_t nextReadMs = 0;
};

// Wakes the STCC4 and starts 1 s continuous measurements
static void stcc4_start(Co2Burst &co2)
{
    stcc4.exitSleepMode();
    co2.running = stcc4.startContinuousMeasurement() == 0;
    // The first result is ready one measurement interval after the start
    co2.nextReadMs = millis() + STCC4_READ_INTERVAL_MS + MEASURE_POLL_MS;
}

static void stcc4_poll(Co2Burst &co2)
{
    if (!co2.running || (int32_t)(millis() - co2.nextReadMs) < 0)
    {
        return;
    }
    // Spaced from the actual read, so a late poll (e.g. after the BME690)
    // does not read the same result twice
    co2.nextReadMs = millis() + STCC4_READ_INTERVAL_MS;

    int16_t ppm = 0;
    float shtTemp = 0.0f;
    float shtRh = 0.0f;
    uint16_t status = 0;
    if (stcc4.readMeasurement(ppm, shtTemp, shtRh, status) != 0)
    {
        return;
    }

    DEBUG_LOG("co2 #%u: %d ppm (SHT45 %.1f C, %.1f %%RH)\n", co2.readings, ppm, shtTemp, shtRh);
    if (co2.readings++ >= CO2_SETTLE_READINGS)
    {
        co2.sum += ppm;
        co2.summed++;
    }
}

// Puts the STCC4 back to sleep and returns the window's mean CO2 (NaN if
// none). stop_continuous_measurement blocks for 1.2 s, so this runs only once
// the BMV080 no longer needs serving.
static float stcc4_finish(Co2Burst &co2)
{
    if (!co2.running)
    {
        return NAN;
    }

    stcc4.stopContinuousMeasurement();
    stcc4.enterSleepMode();

    return co2.summed > 0 ? co2.sum / co2.summed : NAN;
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


// Time of the last successful BME690 reading (0 = none yet since wake-up)
static bool bme690HasReading = false;
static uint32_t lastBme690Ms = 0;

bool bme690_reading_due()
{
    return !bme690HasReading || millis() - lastBme690Ms >= BME690_PERIOD_MS;
}

// The 7semi library always enables the gas heater (300 C) in begin(); clear
// run_gas so a reading does not heat the sensor. The gas value is not used.
void disable_bme690_gas_heater()
{
    Wire.beginTransmission(BME690_ADDR);
    Wire.write(BME690_REG_CTRL_GAS_1);
    Wire.endTransmission(false);
    Wire.requestFrom(BME690_ADDR, (uint8_t)1);
    const uint8_t ctrl = Wire.available() ? Wire.read() : 0;

    Wire.beginTransmission(BME690_ADDR);
    Wire.write(BME690_REG_CTRL_GAS_1);
    Wire.write(ctrl & ~BME690_RUN_GAS_MSK);
    Wire.endTransmission();
}

// One forced measurement (no heater). On failure the previous values are kept
// and a new attempt is made on the next sensor cycle.
void read_bme690(BME690_7semi &bme)
{
    Wire.begin(SDA_PIN0, SCL_PIN0, I2C_FREQ_0);
    if (!bme.begin(Wire))
    {
        return;
    }
    disable_bme690_gas_heater();

    for (uint8_t attempt = 0; attempt < 3; attempt++)
    {
        if (bme.readSensorData())
        {
            ble_temp = bme.getTemperature();
            ble_rh = bme.getHumidity();
            ble_press = bme.getPressure();
            bme690HasReading = true;
            lastBme690Ms = millis();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==================== BMV080 (PM) ====================

// Full-window outputs collected during one window
struct PmBurst
{
    bool running = false;
    uint8_t count = 0;
    float pm1[BMV080_MAX_OUTPUTS];
    float pm25[BMV080_MAX_OUTPUTS];
    float pm10[BMV080_MAX_OUTPUTS];
};

// Median of n values (sorts them in place), NaN if there are none
static float median(float *v, uint8_t n)
{
    if (n == 0)
    {
        return NAN;
    }
    for (uint8_t i = 1; i < n; i++)
    {
        const float x = v[i];
        uint8_t j = i;
        while (j > 0 && v[j - 1] > x)
        {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

static void bmv080_start(SparkFunBMV080SPI &bmv080, bool ready, PmBurst &pm)
{
    pm.running = ready && bmv080.setMode(SF_BMV080_MODE_CONTINUOUS);
}

// The BMV080 outputs a result every second, computed over the last
// BMV080_INTEGRATION_S seconds. Outputs from the first seconds of a burst
// cover a partly empty window and read too low; obstructed ones are invalid.
static void bmv080_poll(SparkFunBMV080SPI &bmv080, PmBurst &pm)
{
    bmv080_output_t out;
    if (!pm.running || !bmv080.readSensor(&out))
    {
        return;
    }

    DEBUG_LOG("pm  %4.1f s: %.1f / %.1f / %.1f ug/m3%s\n", out.runtime_in_sec,
              out.pm1_mass_concentration, out.pm2_5_mass_concentration,
              out.pm10_mass_concentration, out.is_obstructed ? " obstructed" : "");

    if (out.runtime_in_sec < BMV080_INTEGRATION_S || out.is_obstructed ||
        pm.count >= BMV080_MAX_OUTPUTS)
    {
        return;
    }
    pm.pm1[pm.count] = out.pm1_mass_concentration;
    pm.pm25[pm.count] = out.pm2_5_mass_concentration;
    pm.pm10[pm.count] = out.pm10_mass_concentration;
    pm.count++;
}

// Ends the burst and stores the medians. Sent as "no value" unless at least
// one full-window, unobstructed output arrived.
static void bmv080_finish(SparkFunBMV080SPI &bmv080, PmBurst &pm)
{
    if (pm.running)
    {
        // End the measurement before the laser rail is switched off. The
        // SparkFun wrapper has no stop call; a reset returns it to idle.
        bmv080.reset();
    }

    // Per-channel medians keep PM1 <= PM2.5 <= PM10, as every output does
    ble_pm1 = median(pm.pm1, pm.count);
    ble_pm25 = median(pm.pm25, pm.count);
    ble_pm10 = median(pm.pm10, pm.count);
}

// ==================== Schedule (RTC-aligned) ====================
//
// A cycle starts MEASURE_WINDOW_MS before every RTC minute, so its
// notification goes out at hh:mm:00. Between cycles millis() keeps time (it
// runs through light sleep from the 32 kHz crystal) and the RV-3028 gives the
// phase. The RV-3028 has no sub-second register, so the phase is taken from
// the moment its seconds counter ticks over, caught inside every measurement
// window where the CPU is awake anyway.

// millis() at which some RTC minute started (only the phase matters)
static uint32_t rtcMinuteStartMs = 0;

// Rough phase (+-1 s) from a single read, used until a window refines it.
// Without a readable RTC the cycles still run every minute, just unaligned.
void rtc_phase_coarse()
{
    rtcMinuteStartMs = millis();
    if (rtc.updateTime())
    {
        rtcMinuteStartMs -= rtc.getSeconds() * 1000UL;
    }
}

// Watches for the RTC seconds counter to tick over
struct RtcEdge
{
    bool found = false;
    int8_t lastSeconds = -1; // -1 = not read yet
};

static void rtc_phase_poll(RtcEdge &edge)
{
    if (edge.found || !rtc.updateTime())
    {
        return;
    }
    const uint8_t seconds = rtc.getSeconds();
    if (edge.lastSeconds >= 0 && seconds != edge.lastSeconds)
    {
        rtcMinuteStartMs = millis() - seconds * 1000UL;
        edge.found = true;
    }
    edge.lastSeconds = seconds;
}

// Start of the first cycle after afterMs
uint32_t next_cycle_start(uint32_t afterMs)
{
    // rtcMinuteStartMs lies in the past, so this slot is not after afterMs:
    // step forward whole periods (unsigned math also covers the millis() wrap)
    uint32_t slotMs = rtcMinuteStartMs - MEASURE_WINDOW_MS;
    slotMs += ((afterMs - slotMs) / SENSOR_PERIOD_MS + 1) * SENSOR_PERIOD_MS;
    return slotMs;
}

// ==================== Measurement window ====================

// Serves the BMV080 every MEASURE_POLL_MS (Bosch: at least once per second),
// reads the STCC4 once per second and catches the RTC tick for the schedule,
// until endMs. Nothing in here blocks for long.
static void measure_window(SparkFunBMV080SPI &bmv080, PmBurst &pm, Co2Burst &co2,
                           uint32_t endMs)
{
    RtcEdge edge;
    while ((int32_t)(endMs - millis()) > 0)
    {
        bmv080_poll(bmv080, pm);
        stcc4_poll(co2);
        rtc_phase_poll(edge);
        vTaskDelay(pdMS_TO_TICKS(MEASURE_POLL_MS));
    }
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
    // The window ends on an RTC minute; rounding removes the second or so of
    // jitter around it, so every reading is stamped hh:mm:00
    const uint32_t nowEpoch = toEpoch(rtc.getYear(), rtc.getMonth(), rtc.getDate(),
                                      rtc.getHours(), rtc.getMinutes(), rtc.getSeconds());
    p.timestamp = (nowEpoch + 30) / 60 * 60;
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

    // Keep the BMV080 deselected between cycles (its IO side stays powered)
    pinMode(BMV_CS_PIN, OUTPUT);
    digitalWrite(BMV_CS_PIN, HIGH);

    control_sensor_power(false);
}

// One MEASURE_WINDOW_MS cycle: all sensors measure together, then one
// notification is sent
void run_sensor_cycle()
{
    const uint32_t startMs = millis();
    SparkFunBMV080SPI bmv080;
    BME690_7semi bme(0x76, BME690_7semi::MODE_I2C);
    PmBurst pm;
    Co2Burst co2;

    esp_pm_lock_acquire(sensorNoSleepLock);
    esp_pm_lock_acquire(sensorApbLock);

    // CO2 first: every second of the window is one more STCC4 sample
    stcc4_start(co2);

    control_sensor_power(true);

    const bool bmv080Ready = start_sensors(bmv080);

    // Blocking, so before the BMV080 measurement starts (from then on it must
    // be served every second)
    if (bme690_reading_due())
    {
        read_bme690(bme);
    }

    bmv080_start(bmv080, bmv080Ready, pm);
    measure_window(bmv080, pm, co2, startMs + MEASURE_WINDOW_MS - MEASURE_TEARDOWN_MS);
    bmv080_finish(bmv080, pm);

    bmv080.close();

    stop_sensors();

    ble_co2 = stcc4_finish(co2); // blocks 1.2 s, the BMV080 is off by now
    read_max();
    read_rtc();

    DEBUG_LOG("cycle %lu ms: co2 %u of %u readings -> %.0f ppm, pm %u outputs -> %.1f / %.1f / %.1f\n",
              (unsigned long)(millis() - startMs), co2.summed, co2.readings, ble_co2,
              pm.count, ble_pm1, ble_pm25, ble_pm10);

    send_data_ble();

    esp_pm_lock_release(sensorApbLock);
    esp_pm_lock_release(sensorNoSleepLock);
}

void setup()
{
#if WEARAWARE_DEBUG_LOG
    Serial.begin(115200);
#endif

    // Button 1 is left in RTC mode after a deep sleep wake-up
    rtc_gpio_deinit((gpio_num_t)BUTTON_1);

    init_config_peripherals();
    release_off_state_holds();

    // Puts STCC4 and MAX17048 into their sleep modes
    init_i2c_one();

    // Power-on or reset: show "Bluetooth off", then stay fully off until
    // button 1 is pressed
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0)
    {
        ui_power_off();
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
    static bool cycleScheduled = false;
    static uint32_t nextCycleMs = 0;

    // Button presses / BLE changes that arrived meanwhile + screen update
    ui_process_events();

    if (!deviceConnected)
    {
        cycleScheduled = false;
        // Light sleep until a button press or BLE connect wakes us
        ui_wait_for_event(SENSOR_PERIOD_MS);
        return;
    }

    if (!cycleScheduled)
    {
        // Just connected: phase from the RTC, first cycle at the next
        // RTC-aligned slot
        rtc_phase_coarse();
        nextCycleMs = next_cycle_start(millis());
        cycleScheduled = true;
    }

    const int32_t untilNextMs = (int32_t)(nextCycleMs - millis());
    if (untilNextMs > 0)
    {
        // Light sleep until the next cycle, or earlier if button 1 is pressed
        ui_wait_for_event((uint32_t)untilNextMs);
        return;
    }

    // Fixed rate: the next slot follows from the RTC phase, not from when
    // this cycle ends. A slot missed by too much is skipped.
    if ((uint32_t)(-untilNextMs) <= CYCLE_MAX_LATE_MS)
    {
        run_sensor_cycle();
    }
    nextCycleMs = next_cycle_start(millis());
}
