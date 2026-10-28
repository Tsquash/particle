#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include "sensor.h"
#include "screen.h"
#include "spi_bus.h"
#include "spi_pins.h"

#include "font/bold25.h"
#include "font/regular12.h"
#include "bitmaps.h"
#include "pm_link.h"

#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS GxEPD2_213_GDEY0213B74 // GDEY0213B74 250x122, SSD1680

SPIClass epd_spi(FSPI);
// GxEPD2 Definition: 
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))
#define MAX_DISPLAY_BUFFER_SIZE 65536ul
GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(GxEPD2_DRIVER_CLASS(/*CS=*/ EPD_CS, /*DC=*/ EPD_DC, /*RST=*/ EPD_RESET, /*BUSY=*/ EPD_BUSY));

#define PARTIALS_PER_FULL 30

void _draw_quality_indicator(int pos, int pm_val);

int current_partial = 1;
static void (*display_wait_callback)() = nullptr;

static void service_during_display_wait(const void *) {
    if (display_wait_callback) display_wait_callback();
}

bool initialize_spi_bus() {
    static bool spi_initialized = false;

    if (spi_initialized) {
        return true;
    }

    if (!epd_spi.begin(SCK, MISO, MOSI)) {
        Serial.println("[ERROR] Failed to initialize shared SPI bus");
        return false;
    }

    spi_initialized = true;
    return true;
}

bool initialize_display() {
    if (!initialize_spi_bus()) {
        return false;
    }
    display.epd2.selectSPI(epd_spi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    display.init(115200, true, 2, false);
    display.setRotation(0);
    display.fillScreen(GxEPD_WHITE);
    display.display(false);
    // Show logo bitmap
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(12, 60, logo_bitmap, LOGO_WIDTH, LOGO_HEIGHT, GxEPD_BLACK);
    display.display(true);
    return true;
}

void set_display_wait_callback(void (*callback)()) {
    display_wait_callback = callback;
    display.epd2.setBusyCallback(service_during_display_wait);
}

void update_pm(pm data) {
    // update the PM values on the display
    // 1. white out the screen
    // 2. display the three PM headers
    // 3. display the three PM values
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    bool doFull = pmLinkFullRefreshDue() ||
                  (current_partial % PARTIALS_PER_FULL) == 0;
    if(doFull){
        // logic fixes ghosting by blanking the screen white when
        // its time for a full refresh
        display.display(false);
    }
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&regular12);
    display.setCursor(6, 31);
    display.print("PM 1");
    display.setCursor(6, 111);
    display.print("PM 2.5");
    display.setCursor(6, 191);
    display.print("PM 10");
    display.setFont(&bold25);
    display.setCursor(32, 74);
    display.print(min(999, (int)roundf(data.pm1))); // Preserve the three-digit display limit.
    display.setCursor(32, 155);
    display.print(min(999, (int)roundf(data.pm2_5)));
    display.setCursor(32, 235);
    display.print(min(999, (int)roundf(data.pm10)));
    // display the pm quality indicators
    _draw_quality_indicator(1, (int)roundf(data.pm1));
    _draw_quality_indicator(2, (int)roundf(data.pm2_5));
    _draw_quality_indicator(3, (int)roundf(data.pm10));
    display.display(true);
    current_partial++;
}

void _draw_quality_indicator(int pos, int pm_val){
    // if the pm value is above 25, fill a circle
    if(pm_val > 25){
        display.fillCircle(16, 58+((pos-1)*80), 8, GxEPD_BLACK);
        return;
    }
    // every quality gets at least a circle
    display.drawCircle(16, 58+((pos-1)*80), 8, GxEPD_BLACK);
    display.drawCircle(16, 58+((pos-1)*80), 7, GxEPD_BLACK);
    // if it is between 10 and 25, draw a second, unfilled circle
    if(pm_val >10){
        display.fillCircle(16, 58+((pos-1)*80), 3, GxEPD_BLACK);
    }
}

void display_obstructed(){
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(0,0, obstructed_bitmap, 122, 250, GxEPD_BLACK);
    display.display(true);
    current_partial = 0;
}
void display_status(DisplayStatus status) {
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&regular12);
    // 122 x 250
    auto print_centered = [](const char* text, int16_t y) {
        int16_t text_x, text_y;
        uint16_t text_width, text_height;
        display.getTextBounds(text, 0, y, &text_x, &text_y, &text_width, &text_height);
        display.setCursor((display.width() - text_width) / 2 - text_x, y);
        display.print(text);
    };
    switch (status) {
        case DisplayStatus::Pairing:
            display.drawBitmap(29, 50, pairing_bitmap, PAIRING_WIDTH, PAIRING_HEIGHT, GxEPD_BLACK);
            print_centered("Pairing", 135);
            print_centered("with AQM", 163);
            break;
        case DisplayStatus::Paired:
            display.drawBitmap(29, 50, paired_bitmap, PAIRING_WIDTH, PAIRING_HEIGHT, GxEPD_BLACK);
            print_centered("Pairing", 135);
            print_centered("Successful", 163);
            break;
        case DisplayStatus::TimedOut:
            display.drawBitmap(29, 50, pairing_bitmap, PAIRING_WIDTH, PAIRING_HEIGHT, GxEPD_BLACK);
            print_centered("Pairing", 135);
            print_centered("Timed Out", 163);
            break;
        case DisplayStatus::SensorFailure:
            print_centered("Sensor", 117);
            print_centered("Failure", 145);
            break;
    }
    display.display(false);
    current_partial = 0;
}
