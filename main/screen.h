#ifndef SCREEN_H
#define SCREEN_H

#include "sensor.h"

bool initialize_display();
void display_config();
void display_obstructed();
void update_pm(pm data);

#endif