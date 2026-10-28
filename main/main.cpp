#include <Arduino.h>
#include <nvs_flash.h>
#include "sensor.h"
#include "screen.h"
#include "pm_link.h"

SET_LOOP_TASK_STACK_SIZE(60 * 1024);

static constexpr uint8_t AQM_PAIR_BUTTON = 0;
static constexpr uint8_t FACTORY_RESET_BUTTON = 1;
static constexpr uint32_t BOOT_BUTTON_HOLD_MS = 1000;
static constexpr uint32_t SENSOR_SERVICE_INTERVAL_MS = 500;
static constexpr uint16_t MIN_SAMPLES_PER_AVERAGE = 10;

static bool sensorReady = false;
static uint32_t lastSensorAttempt = 0, lastSensorService = 0, lastOutput = 0;

struct PMAccumulator {
    double pm1 = 0;
    double pm2_5 = 0;
    double pm10 = 0;
    uint16_t validSamples = 0;
    uint16_t totalSamples = 0;
    uint16_t obstructedSamples = 0;
};

static PMAccumulator minute{};

static void addSample(const pm &data) {
    ++minute.totalSamples;
    if (data.obstructed) ++minute.obstructedSamples;
    if (data.obstructed || data.invalid) return;
    minute.pm1 += data.pm1;
    minute.pm2_5 += data.pm2_5;
    minute.pm10 += data.pm10;
    ++minute.validSamples;
}

static void serviceSensor() {
    const uint32_t now = millis();
    if (!sensorReady || now - lastSensorService < SENSOR_SERVICE_INTERVAL_MS) return;
    lastSensorService = now;
    pm data{};
    if (read_pm(data)) {
        lastOutput = millis();
        addSample(data);
    }
}

static void performFactoryReset() {
    Serial.println("[RESET] Erasing saved credentials and AQM pairing");
    esp_err_t result = nvs_flash_erase();
    if (result != ESP_OK) {
        Serial.printf("[RESET] Factory reset failed: %s\n", esp_err_to_name(result));
        return;
    }

    // Do not repeatedly erase NVS if the button remains held through the reboot.
    Serial.println("[RESET] Factory reset complete; release button to restart");
    while (digitalRead(FACTORY_RESET_BUTTON) == LOW) delay(10);
    delay(50);
    Serial.flush();
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    pinMode(AQM_PAIR_BUTTON, INPUT_PULLUP);
    pinMode(FACTORY_RESET_BUTTON, INPUT_PULLUP);

    // Confirm either boot action with a continuous one-second hold.
    bool forcePair = digitalRead(AQM_PAIR_BUTTON) == LOW;
    bool factoryReset = digitalRead(FACTORY_RESET_BUTTON) == LOW;
    uint32_t started = millis();
    while ((forcePair || factoryReset) && millis() - started < BOOT_BUTTON_HOLD_MS) {
        forcePair = forcePair && digitalRead(AQM_PAIR_BUTTON) == LOW;
        factoryReset = factoryReset && digitalRead(FACTORY_RESET_BUTTON) == LOW;
        delay(10);
    }

    // Factory reset takes priority if both buttons are held.
    if (factoryReset) performFactoryReset();

    if(!initialize_display()) {
        Serial.println("[DISPLAY] Initialization failed");
    }
    set_display_wait_callback(serviceSensor);
    if (initializePMLink(forcePair)) {
        display_status(DisplayStatus::Pairing);
        started = millis();
        while (!pmLinkPaired() && millis() - started < 120000) {
            processPMLink();
            delay(10);
        }
        finishPMPairing();
        display_status(pmLinkPaired() ? DisplayStatus::Paired : DisplayStatus::TimedOut);
        delay(4000);
    }
    sensorReady = initialize_sensor();
    lastSensorAttempt = millis();
    lastSensorService = millis() - SENSOR_SERVICE_INTERVAL_MS;
    lastOutput = millis();
    if (!sensorReady) display_status(DisplayStatus::SensorFailure);
    Serial.printf("[STARTUP] Sensor %s; normal operation\n", sensorReady ? "ready" : "failed, will retry");
}

void loop() {
    processPMLink();
    uint32_t now = millis();
    if (!sensorReady && now - lastSensorAttempt >= 60000) {
        Serial.println("[SENSOR] Retrying initialization");
        sensorReady = initialize_sensor();
        lastSensorAttempt = millis();
        lastSensorService = millis() - SENSOR_SERVICE_INTERVAL_MS;
        lastOutput = millis();
        minute = {};
        if (!sensorReady) display_status(DisplayStatus::SensorFailure);
    }
    // Bosch requires service at least once per second in continuous mode. Poll at
    // 500 ms for scheduling margin; the sensor itself produces output at 0.97 Hz.
    serviceSensor();
    if (sensorReady && millis() - lastOutput > 30000) {
        Serial.println("[SENSOR] No output for 30 seconds; scheduling reinitialization");
        sensorReady = false;
        lastSensorAttempt = millis() - 60000;
        display_status(DisplayStatus::SensorFailure);
    }
    if (pmMeasurementDue()) {
        const uint16_t sampleCount = minute.validSamples;
        const uint16_t totalCount = minute.totalSamples;
        const bool fullyObstructed = totalCount > 0 && minute.obstructedSamples == totalCount;
        const bool valid = sampleCount >= MIN_SAMPLES_PER_AVERAGE;
        pm averaged{};
        if (valid) {
            averaged.pm1 = minute.pm1 / sampleCount;
            averaged.pm2_5 = minute.pm2_5 / sampleCount;
            averaged.pm10 = minute.pm10 / sampleCount;
            PMReading reading{(uint16_t)lroundf(averaged.pm1),
                              (uint16_t)lroundf(averaged.pm2_5),
                              (uint16_t)lroundf(averaged.pm10)};
            Serial.printf("[PM] %u-sample average: PM1=%.1f PM2.5=%.1f PM10=%.1f ug/m3\n",
                          sampleCount, averaged.pm1, averaged.pm2_5, averaged.pm10);
            sendPMReading(reading); // Before the blocking EPD refresh.
        }

        // Close the window at the scheduled deadline. Samples after the display
        // refresh belong to the next minute and are never mixed with this one.
        minute = {};
        if (valid) {
            update_pm(averaged);
        } else if (fullyObstructed) {
            display_obstructed();
        } else {
            Serial.printf("[PM] Only %u/%u valid samples this minute; need %u, no update sent\n",
                          sampleCount, totalCount, MIN_SAMPLES_PER_AVERAGE);
        }
    }
    delay(10);
}
