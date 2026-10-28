#ifndef SCREEN_H
#define SCREEN_H

#include "sensor.h"

enum class DisplayStatus {
    Pairing,
    Paired,
    TimedOut,
    SensorFailure,
};

bool initialize_display();
void set_display_wait_callback(void (*callback)());
void display_status(DisplayStatus status);
void display_obstructed();
void update_pm(pm data);

#endif
