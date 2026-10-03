#pragma once
// WearAware e-paper user interface (200x200 B/W, GDEY0154D67)
//
// Screens:
//   PAIRING     - advertising, shows how to connect from the phone
//   CONNECTED   - "BLE connected, check your phone"
//   CONFIRM     - "Disconnect phone?" (B2 = yes, B3 = no, times out)
//   BLE_OFF     - user turned Bluetooth off, B1 turns it back on
//
// Power strategy:
//   - The screen is only redrawn when the screen state changes. Between
//     redraws the panel is hibernated and its supply (EPD_CONTROL) is cut,
//     e-paper keeps the image without power.
//   - BUTTON_1 is a GPIO light-sleep wake source + interrupt, so a press
//     wakes the chip and posts an event to the loop task.
//   - BUTTON_2 / BUTTON_3 cannot wake the chip, so they are only polled
//     inside the short confirmation window opened by BUTTON_1.

#include "AppConfig.h"
#include "images.h"

#include <SPI.h>
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "freertos/queue.h"

#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

GxEPD2_BW<GxEPD2_154_GDEY0154D67, GxEPD2_154_GDEY0154D67::HEIGHT>
    display(GxEPD2_154_GDEY0154D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

enum UiScreen : uint8_t
{
    SCREEN_NONE = 0,
    SCREEN_PAIRING,
    SCREEN_CONNECTED,
    SCREEN_CONFIRM_DISCONNECT,
    SCREEN_BLE_OFF
};

enum UiEvent : uint8_t
{
    UI_EVT_BUTTON1 = 1,
    UI_EVT_BLE_CHANGED
};

constexpr uint32_t UI_CONFIRM_TIMEOUT_MS = 10000;
constexpr uint32_t UI_DEBOUNCE_MS = 30;
constexpr uint32_t UI_POLL_MS = 20;

static QueueHandle_t uiEventQueue = nullptr;
static UiScreen uiCurrentScreen = SCREEN_NONE;
static esp_pm_lock_handle_t uiNoSleepLock = nullptr;
static esp_pm_lock_handle_t uiApbLock = nullptr;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Safe to call from any task (e.g. NimBLE callbacks)
void ui_post_event(UiEvent evt)
{
    if (uiEventQueue != nullptr)
    {
        xQueueSend(uiEventQueue, &evt, 0);
    }
}

// Level-low interrupt on BUTTON_1 (same config as the light-sleep wake source).
// Disable it here so a held button does not retrigger; ui_rearm_button1()
// turns it back on once the button is released.
static void IRAM_ATTR button1_isr(void *)
{
    gpio_intr_disable((gpio_num_t)BUTTON_1);

    if (uiEventQueue != nullptr)
    {
        UiEvent evt = UI_EVT_BUTTON1;
        BaseType_t higherPrioWoken = pdFALSE;
        xQueueSendFromISR(uiEventQueue, &evt, &higherPrioWoken);
        if (higherPrioWoken)
        {
            portYIELD_FROM_ISR();
        }
    }
}

static void ui_rearm_button1()
{
    gpio_wakeup_enable((gpio_num_t)BUTTON_1, GPIO_INTR_LOW_LEVEL);
    gpio_intr_enable((gpio_num_t)BUTTON_1);
}

static bool ui_button_pressed(uint8_t pin)
{
    return digitalRead(pin) == LOW;
}

static void ui_wait_release(uint8_t pin)
{
    while (ui_button_pressed(pin))
    {
        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(UI_DEBOUNCE_MS));
}

// ---------------------------------------------------------------------------
// Display power / SPI handling
// The SPI bus is shared with the BMV080, which run_sensor_cycle() starts and
// stops. The UI only draws from the loop task, so the two never overlap.
// ---------------------------------------------------------------------------

static void epd_power_on()
{
    digitalWrite(EPD_CONTROL, EPD_POWER_ON);
    vTaskDelay(pdMS_TO_TICKS(100));

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, -1);
    display.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    // Panel lost power since the last draw, so always do a full init
    display.init(0, true, 2, false);
    display.setRotation(0);
}

static void epd_power_off()
{
    display.hibernate();
    display.epd2.end(); // SPI.end() + CS/DC/RST as inputs

    pinMode(MOSI_PIN, INPUT);
    pinMode(SCK_PIN, INPUT);
    pinMode(MISO_PIN, INPUT);

    digitalWrite(EPD_CONTROL, EPD_POWER_OFF);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// Draws an XBM (LSB first) bitmap, every pixel scaled to a s x s block
static void draw_xbm_scaled(int16_t x, int16_t y, const uint8_t *bmp,
                            int16_t w, int16_t h, uint8_t s, uint16_t color)
{
    const int16_t bytesPerRow = (w + 7) / 8;
    for (int16_t j = 0; j < h; j++)
    {
        for (int16_t i = 0; i < w; i++)
        {
            uint8_t b = pgm_read_byte(bmp + j * bytesPerRow + i / 8);
            if (b & (1 << (i & 7)))
            {
                display.fillRect(x + i * s, y + j * s, s, s, color);
            }
        }
    }
}

static int16_t text_width(const char *text, const GFXfont *font)
{
    int16_t x1, y1;
    uint16_t w, h;
    display.setFont(font);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w + x1;
}

// Centered text on baseline y. Falls back to the small regular font if
// the text does not fit on one line.
static void print_centered(const char *text, int16_t y, const GFXfont *font,
                           uint16_t color = GxEPD_BLACK)
{
    int16_t w = text_width(text, font);
    if (w > display.width() - 4)
    {
        font = &FreeSans9pt7b;
        w = text_width(text, font);
    }
    display.setFont(font);
    display.setTextColor(color);
    display.setCursor((display.width() - w) / 2, y);
    display.print(text);
}

static void print_left(const char *text, int16_t x, int16_t y, const GFXfont *font)
{
    display.setFont(font);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(x, y);
    display.print(text);
}

// Icons are 15x15 XBM
constexpr int16_t ICON_SIZE = 15;

static void draw_header(bool bleOn)
{
    print_left("WearAware", 6, 24, &FreeSansBold12pt7b);
    draw_xbm_scaled(display.width() - 2 * ICON_SIZE - 6, 3,
                    bleOn ? ble_on : ble_off, ICON_SIZE, ICON_SIZE, 2, GxEPD_BLACK);
    display.fillRect(0, 36, display.width(), 2, GxEPD_BLACK);
}

// Inverted footer bar used for button hints
static void draw_footer(const char *hint)
{
    display.fillRect(0, 172, display.width(), 28, GxEPD_BLACK);
    print_centered(hint, 192, &FreeSansBold9pt7b, GxEPD_WHITE);
}

static void draw_big_icon(const uint8_t *icon)
{
    const uint8_t scale = 3;
    draw_xbm_scaled((display.width() - ICON_SIZE * scale) / 2, 46,
                    icon, ICON_SIZE, ICON_SIZE, scale, GxEPD_BLACK);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static void draw_screen_pairing()
{
    draw_header(false);
    print_centered("Connect to phone", 60, &FreeSansBold9pt7b);
    print_left("1. Open the app", 8, 86, &FreeSans9pt7b);
    print_left("2. Scan for devices", 8, 108, &FreeSans9pt7b);
    print_left("3. Select:", 8, 130, &FreeSans9pt7b);
    print_centered(BLE_DEVICE_NAME, 156, &FreeSansBold9pt7b);
    draw_footer("B1: Bluetooth off");
}

static void draw_screen_connected()
{
    draw_header(true);
    draw_big_icon(ble_on);
    print_centered("Connected", 122, &FreeSansBold12pt7b);
    print_centered("Check your phone", 150, &FreeSans9pt7b);
    draw_footer("B1: Disconnect");
}

static void draw_screen_confirm()
{
    draw_header(true);
    print_centered("Disconnect", 72, &FreeSansBold12pt7b);
    print_centered("phone?", 100, &FreeSansBold12pt7b);
    print_centered("B2: Yes", 132, &FreeSansBold9pt7b);
    print_centered("B3: No", 156, &FreeSansBold9pt7b);
    draw_footer("Cancels in 10 s");
}

static void draw_screen_ble_off()
{
    draw_header(false);
    draw_big_icon(ble_off);
    print_centered("Bluetooth off", 122, &FreeSansBold12pt7b);
    print_centered("Phone disconnected", 150, &FreeSans9pt7b);
    draw_footer("B1: Pair again");
}

static void ui_render(UiScreen screen)
{
    if (screen == uiCurrentScreen || screen == SCREEN_NONE)
    {
        return;
    }

    // Keep the CPU awake and APB at full speed while SPI + BUSY waits run
    esp_pm_lock_acquire(uiNoSleepLock);
    esp_pm_lock_acquire(uiApbLock);

    epd_power_on();

    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
        switch (screen)
        {
        case SCREEN_PAIRING:
            draw_screen_pairing();
            break;
        case SCREEN_CONNECTED:
            draw_screen_connected();
            break;
        case SCREEN_CONFIRM_DISCONNECT:
            draw_screen_confirm();
            break;
        case SCREEN_BLE_OFF:
            draw_screen_ble_off();
            break;
        default:
            break;
        }
    } while (display.nextPage());

    epd_power_off();

    esp_pm_lock_release(uiApbLock);
    esp_pm_lock_release(uiNoSleepLock);

    uiCurrentScreen = screen;
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

// Screen that matches the current BLE state
static UiScreen ui_target_screen()
{
    if (deviceConnected)
    {
        return SCREEN_CONNECTED;
    }
    return bleUserDisabled ? SCREEN_BLE_OFF : SCREEN_PAIRING;
}

void ui_sync_screen()
{
    ui_render(ui_target_screen());
}

static void ble_disconnect_by_user()
{
    // Set first so onDisconnect() does not restart advertising
    bleUserDisabled = true;
    NimBLEDevice::stopAdvertising();

    for (uint16_t connId : pServer->getPeerDevices())
    {
        pServer->disconnect(connId);
    }

    // Wait for the disconnect callback so the next screen is correct
    const uint32_t startMs = millis();
    while (deviceConnected && millis() - startMs < 3000)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ble_enable_by_user()
{
    bleUserDisabled = false;
    NimBLEDevice::startAdvertising();
}

// Shows the confirmation screen and polls B1/B2/B3.
// Returns true if the user confirmed the disconnect.
static bool ui_ask_disconnect()
{
    ui_render(SCREEN_CONFIRM_DISCONNECT);

    // Buttons 2/3 cannot wake the chip, so stay awake while waiting
    esp_pm_lock_acquire(uiNoSleepLock);

    bool confirmed = false;
    const uint32_t startMs = millis();

    while (millis() - startMs < UI_CONFIRM_TIMEOUT_MS)
    {
        if (!deviceConnected)
        {
            break; // phone left on its own
        }

        if (ui_button_pressed(BUTTON_2))
        {
            vTaskDelay(pdMS_TO_TICKS(UI_DEBOUNCE_MS));
            if (ui_button_pressed(BUTTON_2))
            {
                ui_wait_release(BUTTON_2);
                confirmed = true;
                break;
            }
        }

        if (ui_button_pressed(BUTTON_3) || ui_button_pressed(BUTTON_1))
        {
            vTaskDelay(pdMS_TO_TICKS(UI_DEBOUNCE_MS));
            if (ui_button_pressed(BUTTON_3) || ui_button_pressed(BUTTON_1))
            {
                ui_wait_release(BUTTON_3);
                ui_wait_release(BUTTON_1);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }

    esp_pm_lock_release(uiNoSleepLock);
    return confirmed;
}

static void ui_handle_button1()
{
    // Debounce: ignore glitches shorter than UI_DEBOUNCE_MS
    vTaskDelay(pdMS_TO_TICKS(UI_DEBOUNCE_MS));
    if (!ui_button_pressed(BUTTON_1))
    {
        ui_rearm_button1();
        return;
    }
    ui_wait_release(BUTTON_1);

    switch (ui_target_screen())
    {
    case SCREEN_CONNECTED:
        if (ui_ask_disconnect())
        {
            ble_disconnect_by_user();
        }
        break;

    case SCREEN_PAIRING:
        bleUserDisabled = true;
        NimBLEDevice::stopAdvertising();
        break;

    case SCREEN_BLE_OFF:
        ble_enable_by_user();
        break;

    default:
        break;
    }

    // Drop presses that queued up while we were busy, then listen again
    UiEvent evt;
    while (xQueuePeek(uiEventQueue, &evt, 0) == pdTRUE && evt == UI_EVT_BUTTON1)
    {
        xQueueReceive(uiEventQueue, &evt, 0);
    }
    ui_rearm_button1();
}

static void ui_handle_event(UiEvent evt)
{
    if (evt == UI_EVT_BUTTON1)
    {
        ui_handle_button1();
    }
    // UI_EVT_BLE_CHANGED needs no action here, ui_sync_screen() reacts to it
}

// Handles everything already queued, then updates the screen
void ui_process_events()
{
    UiEvent evt;
    while (xQueueReceive(uiEventQueue, &evt, 0) == pdTRUE)
    {
        ui_handle_event(evt);
    }
    ui_sync_screen();
}

// Sleeps (light sleep allowed) until a UI event arrives or timeout expires
void ui_wait_for_event(uint32_t timeoutMs)
{
    UiEvent evt;
    if (xQueueReceive(uiEventQueue, &evt, pdMS_TO_TICKS(timeoutMs)) == pdTRUE)
    {
        ui_handle_event(evt);
        ui_process_events();
    }
}

// Call once at the end of setup(), after esp_pm_configure()
void ui_init()
{
    uiEventQueue = xQueueCreate(8, sizeof(UiEvent));

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui_no_sleep", &uiNoSleepLock);
    esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "ui_apb", &uiApbLock);

    pinMode(BUTTON_2, INPUT_PULLUP);
    pinMode(BUTTON_3, INPUT_PULLUP);

    // BUTTON_1: wake source from light sleep + interrupt when awake.
    // Keep its normal config (pull-up, input) during light sleep.
    gpio_sleep_sel_dis((gpio_num_t)BUTTON_1);
    gpio_install_isr_service(0); // ESP_ERR_INVALID_STATE if already installed: fine
    gpio_isr_handler_add((gpio_num_t)BUTTON_1, button1_isr, nullptr);
    esp_sleep_enable_gpio_wakeup();
    ui_rearm_button1();

    // First screen (pairing, since advertising already started)
    ui_sync_screen();
}
