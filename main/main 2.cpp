#include <Arduino.h>
#include "sensor.h"
#include "screen.h"

// Increase stack size for Bosch BMV080
#if defined(ESP32)
SET_LOOP_TASK_STACK_SIZE(60 * 1024);
#endif

void setup() {
    Serial.begin(115200);
    delay(3000);

    Serial.println("\n[STARTUP] PM sensor bring-up");

    initialize_display();
    
    if (!initialize_sensor()) {
        Serial.println("[STARTUP] BMV080 initialization failed");
        while(1);
    } else {
        Serial.println("[STARTUP] BMV080 initialization complete");
    }
    Serial.println("[STARTUP] Setup complete\n");
}

bool firstObstruction = false; 

void loop() {
    
    pm data;
    if (!read_pm(data)){
        Serial.println("[ERROR] Failed to read PM data");

    }
    else if(data.obstructed & firstObstruction){
        // display obstruction message after second consecutive obstruction
        Serial.println("[ERROR] PM sensor is obstructed");
        display_obstructed();
        // wait for obstruction to clear
        do {
            delay(1000);
            read_pm(data);
        } while(data.obstructed);
        // skip to the next loop iteration
        return;
    }
    else if(data.obstructed){
        firstObstruction = true; // set flag on first obstruction
        Serial.println("[ERROR] PM sensor is obstructed for the first consecutive time");
    }
    else {
        firstObstruction = false; // clear flag on first successful read
        update_pm(data);
    }
    delay(5000);
}
