#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>
#include <SparkFun_BMV080_Arduino_Library.h>
#include "spi_bus.h"

struct pm {
    float pm1;
    float pm2_5;
    float pm10;
    bool obstructed;
    bool invalid; // negative, non-finite or outside the sensor range
}; typedef struct pm pm_t;

bool initialize_sensor();
bool read_pm(pm &data);
bool pause_sensor();
bool resume_sensor();
bool read_outdoor_pm(pm &outdoor_data);

#endif