#include "sensor.h"
#include "spi_pins.h"

SparkFunBMV080SPI sensor;

bool initialize_sensor() {
    // Close a previous failed attempt before reopening the Bosch driver.
    sensor.close();
    pinMode(PM_CS, OUTPUT);
    digitalWrite(PM_CS, HIGH);

    if (!initialize_spi_bus()) {
        return false;
    }

    static bool busReady = false;
    if (!busReady && sensor.begin(PM_CS, epd_spi) == false) {
        Serial.println("PM sensor SPI init failure. Check your jumpers and the hookup guide.");
        return false;
    }
    busReady = true;
    Serial.println("BMV080 SPI init successful");

    /* Initialize the Sensor (read driver, open, reset, id etc.)*/
    if (!sensor.init()) {
        Serial.println("BMV080 init failure");
        return false;
    }
    Serial.println("BMV080 init successful");

    /* Set the sensor mode to continuous mode */
    if(sensor.setMode(SF_BMV080_MODE_CONTINUOUS) == true)
    {
        Serial.println("BMV080 set to continuous mode");
    }
    else
    {
        Serial.println("Error setting BMV080 mode");
        return false;
    }
    return true;
}

bool read_pm(pm &data) {
    bmv080_output_t output{};
    if (!sensor.readSensor(&output)) return false; // No new output yet.
    data.pm1 = output.pm1_mass_concentration;
    data.pm2_5 = output.pm2_5_mass_concentration;
    data.pm10 = output.pm10_mass_concentration;
    data.obstructed = output.is_obstructed;
    data.invalid = output.is_outside_measurement_range ||
                   !isfinite(data.pm1) || !isfinite(data.pm2_5) || !isfinite(data.pm10) ||
                   data.pm1 < 0 || data.pm2_5 < 0 || data.pm10 < 0 ||
                   data.pm1 > 1000 || data.pm2_5 > 1000 || data.pm10 > 1000;
    return true;
}

bool read_outdoor_pm(pm &outdoor_data) {
    return false;
}

bool pause_sensor() { return sensor.stopMeasurement(); }
bool resume_sensor() { return sensor.setMode(SF_BMV080_MODE_CONTINUOUS); }
