#include <Arduino.h>
#include "sensor.h"
#include "screen.h"
#include "pm_link.h"

SET_LOOP_TASK_STACK_SIZE(60 * 1024);

static bool sensorReady = false;
static uint32_t lastSensorAttempt = 0, lastOutput = 0;
static pm latest{};
static bool haveOutput = false;
static bool awaitingMeasurement = false;
static uint32_t measurementStarted = 0;

void setup() {
    Serial.begin(115200);
    pinMode(0, INPUT_PULLUP);
    pinMode(1, INPUT_PULLUP);
    // Match AQM's one-second boot hold; capture before display initialization.
    bool forcePair = digitalRead(0) == LOW;
    uint32_t started = millis();
    while (forcePair && millis() - started < 1000) {
        forcePair = digitalRead(0) == LOW;
        delay(10);
    }
    initialize_display();
    if (initializePMLink(forcePair)) {
        display_status("Pairing\nwith AQM...");
        started = millis();
        while (!pmLinkPaired() && millis() - started < 120000) {
            processPMLink();
            delay(10);
        }
        finishPMPairing();
        display_status(pmLinkPaired() ? "Pairing\nsuccessful" : "Pairing\ntimed out");
        delay(4000);
    }
    sensorReady = initialize_sensor();
    lastSensorAttempt = millis();
    lastOutput = millis();
    Serial.printf("[STARTUP] Sensor %s; normal operation\n", sensorReady ? "ready" : "failed, will retry");
}

void loop() {
    processPMLink();
    uint32_t now = millis();
    if (!sensorReady && now - lastSensorAttempt >= 60000) {
        Serial.println("[SENSOR] Retrying initialization");
        sensorReady = initialize_sensor();
        lastSensorAttempt = millis();
        lastOutput = millis();
        haveOutput = false;
    }
    // Bosch requires service at least once per second in continuous mode.
    // Keep servicing while awake; only display/transmit one selected sample per minute.
    pm data{};
    if (sensorReady && read_pm(data)) {
        latest = data;
        lastOutput = millis();
        haveOutput = true;
    }
    if (sensorReady && millis() - lastOutput > 30000) {
        Serial.println("[SENSOR] No output for 30 seconds; scheduling reinitialization");
        sensorReady = false;
        haveOutput = false;
        lastSensorAttempt = millis() - 60000;
    }
    if (pmMeasurementDue()) {
        awaitingMeasurement = true;
        measurementStarted = millis();
    }
    if (awaitingMeasurement) {
        // Wait for an output produced at/after the target, never replay a cached sample.
        bool fresh = haveOutput && (int32_t)(lastOutput - measurementStarted) >= 0;
        bool valid = fresh && !latest.obstructed && !latest.invalid;
        if (valid) {
            PMReading reading{(uint16_t)lroundf(latest.pm1), (uint16_t)lroundf(latest.pm2_5), (uint16_t)lroundf(latest.pm10)};
            latest.pm1 = reading.pm1;
            latest.pm2_5 = reading.pm25;
            latest.pm10 = reading.pm10;
            Serial.printf("[PM] PM1=%u PM2.5=%u PM10=%u ug/m3\n", reading.pm1, reading.pm25, reading.pm10);
            sendPMReading(reading); // Before the blocking EPD refresh.
        }
        if (valid || millis() - measurementStarted >= 5000) {
            // EPD waits exceed Bosch's service interval. Pause measurement only,
            // keep the ESP32 awake, and restart the stream after drawing.
            if (sensorReady && !pause_sensor()) sensorReady = false;
            if (valid) update_pm(latest);
            else {
                Serial.println("[PM] No valid sample this minute; no BLE reading sent");
                if (haveOutput && latest.obstructed) display_obstructed();
                else display_status("Sensor\nnot ready");
            }
            if (sensorReady) sensorReady = resume_sensor();
            if (!sensorReady) Serial.println("[SENSOR] Stream unavailable; will reinitialize");
            haveOutput = false;
            lastOutput = millis();
            awaitingMeasurement = false;
        }
    }
    delay(10);
}
