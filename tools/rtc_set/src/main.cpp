// WearAware - RTC setter
//
// Asks for year, month, day, hour, minute and second in the serial monitor,
// writes them to the RV-3028-C7, then prints the RTC time once per second.

#include <Arduino.h>
#include <Wire.h>
#include <RV-3028-C7.h>

// I2C1 -> STCC4, MAX17048, RTC (same as AppConfig.h)
static constexpr uint8_t SDA_PIN1 = 8;
static constexpr uint8_t SCL_PIN1 = 9;

RV3028 rtc;

// Waits for a line from the serial monitor and returns it as a number
// within [minVal, maxVal]. Asks again on bad input.
int askNumber(const char *label, int minVal, int maxVal)
{
    while (true)
    {
        Serial.printf("%s (%d-%d): ", label, minVal, maxVal);

        while (!Serial.available())
            delay(10);
        String line = Serial.readStringUntil('\n');
        line.trim();

        const int value = line.toInt();
        if (line.length() > 0 && value >= minVal && value <= maxVal)
        {
            Serial.println(value);
            return value;
        }
        Serial.println("invalid, try again");
    }
}

int daysInMonth(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return (month == 2 && leap) ? 29 : days[month - 1];
}

// 0 = Sunday ... 6 = Saturday
int dayOfWeek(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void setup()
{
    Serial.begin(115200);
    while (!Serial)
        delay(10);
    delay(500);

    Wire.begin(SDA_PIN1, SCL_PIN1);

    while (!rtc.begin(Wire))
    {
        Serial.println("RV-3028 not found on SDA 8 / SCL 9, retrying...");
        delay(1000);
    }

    rtc.updateTime();
    Serial.printf("\nCurrent RTC time: %s\n\n", rtc.stringTimeStamp());
    Serial.println("Enter the new date and time:");

    const int year = askNumber("Year", 2000, 2099);
    const int month = askNumber("Month", 1, 12);
    const int day = askNumber("Day", 1, daysInMonth(year, month));
    const int hour = askNumber("Hour", 0, 23);
    const int minute = askNumber("Minute", 0, 59);
    const int second = askNumber("Second", 0, 59);

    if (rtc.setTime(second, minute, hour, dayOfWeek(year, month, day), day, month, year))
        Serial.println("\nRTC set.\n");
    else
        Serial.println("\nSetting the RTC failed!\n");
}

void loop()
{
    rtc.updateTime();
    Serial.println(rtc.stringTimeStamp());
    delay(1000);
}