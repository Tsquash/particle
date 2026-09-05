#include "sensor.h"
#include "spi_pins.h"

SparkFunBMV080SPI sensor;

bool initialize_sensor() {
    pinMode(PM_CS, OUTPUT);
    digitalWrite(PM_CS, HIGH);

    if (!initialize_spi_bus()) {
        return false;
    }

    if (sensor.begin(PM_CS, epd_spi) == false) {
        Serial.println("PM sensor SPI init failure. Check your jumpers and the hookup guide.");
        return false;
    }
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
    if(sensor.readSensor())
    {
        float pm2_5 = sensor.PM25();
        float pm1 = sensor.PM1();
        float pm10 = sensor.PM10();

        // Display only has room for 3 digits before the decimal point.
        data.pm2_5 = pm2_5 > 999.0f ? 999.0f : pm2_5;
        data.pm1 = pm1 > 999.0f ? 999.0f : pm1;
        data.pm10 = pm10 > 999.0f ? 999.0f : pm10;

        Serial.printf("PM1: %.2f\n", data.pm1);
        Serial.printf("PM2.5: %.2f\n", data.pm2_5);
        Serial.printf("PM10: %.2f\n", data.pm10);

        if(sensor.isObstructed())
        {
            data.obstructed = true;
            Serial.print("\tObstructed");
        }
        else
        {
            data.obstructed = false;
        }

        Serial.println();
        return true;
    }
    else return false;
}

bool read_outdoor_pm(pm &outdoor_data){
    
}