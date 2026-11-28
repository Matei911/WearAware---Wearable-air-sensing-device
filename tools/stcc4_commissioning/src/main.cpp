// WearAware - STCC4 commissioning
//
// Sensirion's continuous-measurement example for the STCC4, set up for the
// WearAware board: I2C1 on SDA 8 / SCL 9, address 0x64. Prints CO2 once per
// second, plus the temperature / humidity the STCC4 reads from the SHT45
// it uses for compensation.
//
// The first readings after start-up are not yet settled; let it run a few
// minutes before judging the CO2 value.

#include <Arduino.h>
#include <SensirionI2cStcc4.h>
#include <Wire.h>

// make sure that we use the proper definition of NO_ERROR
#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

// I2C1 -> STCC4, MAX17048, RTC (same as AppConfig.h)
static constexpr uint8_t SDA_PIN1 = 8;
static constexpr uint8_t SCL_PIN1 = 9;
static constexpr uint32_t I2C_FREQ_1 = 100000;

// Pins kept in their safe state while this tool runs (same as main firmware)
static constexpr uint8_t EN_LDO = 1;       // sensor laser/analog rail: off
static constexpr uint8_t EPD_CONTROL = 37; // screen power (active LOW): off
static constexpr uint8_t BMV_CS_PIN = 36;  // BMV080 chip select: deselected

SensirionI2cStcc4 sensor;

static char errorMessage[64];
static int16_t error;

static void printError(const char *what)
{
    errorToString(error, errorMessage, sizeof errorMessage);
    Serial.printf("Error trying to execute %s(): %s\n", what, errorMessage);
}

void setup()
{
    pinMode(EN_LDO, OUTPUT);
    digitalWrite(EN_LDO, LOW);
    pinMode(EPD_CONTROL, OUTPUT);
    digitalWrite(EPD_CONTROL, HIGH);
    pinMode(BMV_CS_PIN, OUTPUT);
    digitalWrite(BMV_CS_PIN, HIGH);

    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 5000)
        delay(100);

    Wire.begin(SDA_PIN1, SCL_PIN1);
    Wire.setClock(I2C_FREQ_1);
    sensor.begin(Wire, STCC4_I2C_ADDR_64);

    delay(6);

    // The main firmware leaves the STCC4 in sleep mode. If it is already
    // awake this NACKs, which is expected, so the error is not reported.
    sensor.exitSleepMode();

    // Make sure no measurement from a previous run is still going
    error = sensor.stopContinuousMeasurement();
    if (error != NO_ERROR)
        printError("stopContinuousMeasurement");

    uint32_t productId = 0;
    uint64_t serialNumber = 0;
    error = sensor.getProductId(productId, serialNumber);
    if (error != NO_ERROR)
    {
        printError("getProductId");
        Serial.println("STCC4 not answering at 0x64 on SDA 8 / SCL 9");
    }
    else
    {
        Serial.printf("productId: 0x%08lX\tserialNumber: 0x%08lX%08lX\n",
                      (unsigned long)productId,
                      (unsigned long)(serialNumber >> 32),
                      (unsigned long)(serialNumber & 0xFFFFFFFFUL));
    }

    error = sensor.startContinuousMeasurement();
    if (error != NO_ERROR)
    {
        printError("startContinuousMeasurement");
        return;
    }

    Serial.println("Continuous measurement started (1 s interval)");
}

void loop()
{
    int16_t co2Concentration = 0;
    float temperature = 0.0f;
    float relativeHumidity = 0.0f;
    uint16_t sensorStatus = 0;

    delay(1000);

    error = sensor.readMeasurement(co2Concentration, temperature,
                                   relativeHumidity, sensorStatus);
    if (error != NO_ERROR)
    {
        printError("readMeasurement");
        return;
    }

    Serial.printf("CO2: %d ppm\tT: %.2f C\tRH: %.2f %%\tstatus: 0x%04X\n",
                  co2Concentration, temperature, relativeHumidity, sensorStatus);
}
