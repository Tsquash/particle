# particle: A Particulate Matter Sensor

particle is an ESP32-C6-based particulate matter sensor. It measures PM1.0,
PM2.5, and PM10, displays the latest measurements locally via an e-paper display, and can send them to
a paired [AQM](https://github.com/Tsquash/AirQualityMonitor) (air quality monitor) over Bluetooth Low Energy (BLE).
The AQM is the Matter-connected device. This device acts as its PM sensor
peripheral and does not commission itself as a separate Matter device.

## Hardware

- **MCU:** Seeed Studio XIAO ESP32-C6
- **Particulate sensor:** DFRobot Fermion BMV080, connected over SPI and driven with the
  SparkFun BMV080 Arduino library
- **Display:** 2.13", 250 x 122 pixel monochrome e-paper display using the
  GDEY0213B74 panel/SSD1680 driver, driven with GxEPD2 and Adafruit GFX
- **PCB** Custom particle PCB, can be found in /KICAD directory

## Operating modes

### Standalone / Normal Operation

The PM sensor starts collecting data even when no AQM is available. It polls
the BMV080 for new output approximately every 500 ms and forms one-minute
averages. Measurements that are obstructed or outside the valid range are
not included in the average. When all samples in an interval indicate obstruction,
the display shows an obstructed status on the screen instead of a numerical reading.

Standalone operation does not require BLE or a network connection. The first
boot makes one automatic attempt to pair with an AQM; the attempt ends after
2 minutes have elapsed, then the device runs standalone until pairing is explicitly
requested again with the pairing button or a factory reset.

### Linked to an AQM

The PM sensor pairs with the companion Matter AQM over BLE. The AQM supplies
the measurement schedule and its RTC time; the PM sensor sends its one-minute
PM averages to the AQM when a BLE connection and notification subscription
are available. The AQM can then use the readings in its air-quality matter
cluster data. The PM sensor continues to take and display local readings if
the AQM is temporarily disconnected.

Pairing is initiated automatically on the first boot. Put the AQM into its
corresponding PM-pairing mode while the PM sensor displays its pairing screen.
The pairing window lasts up to 120 seconds. On success, the peer is saved in
non-volatile storage and the PM sensor will try to reconnect to that AQM on
subsequent boots. If pairing times out, the PM sensor continues in standalone
mode; hold the pairing button for one second during a later boot to try again.

## Buttons

Both buttons are checked at boot. Hold the button
continuously for at least one second to invoke its action.

| Button | Location | GPIO | Action |
| --- | --- |---: | --- |
| AQM pair | Closest to back opening | 0 | Force a new pairing attempt, replacing the previously saved AQM peer |
| Factory reset | Further from back opening | 1 | Erase non-volatile storage, including the saved pairing state, and restart |

## Build, upload, and serial output

Install [PlatformIO](https://platformio.org/) and the Espressif platform
dependencies, connect the XIAO ESP32-C6, then run these commands from the
project directory:

```sh
pio run -e esp32c6
pio run -e esp32c6 -t upload
pio device monitor -b 115200
```

The PlatformIO environment is defined in [`platformio.ini`](platformio.ini).
The project uses the Arduino and ESP-IDF frameworks and is configured for the
`seeed_xiao_esp32c6` board. Serial diagnostics run at 115200 baud. Upload port
selection may need to be configured for the host computer.

## Project layout

```text
main/                  Firmware source, display assets, and project component config
  font/                Embedded Adafruit GFX fonts
lib/                   Project-local libraries (including modified GxEPD2 and BMV080 libs)
managed_components/    ESP-IDF managed components
KICAD/                 PCB and schematic design files
2D/                    Drawings
3D/                    Board, component, and enclosure models
UI/                    Display/UI mockups and PCB graphics
.vscode/               VS Code project configuration
platformio.ini         PlatformIO build environment
sdkconfig.defaults.*   ESP-IDF default configuration for the target
huge_app.csv           ESP-IDF flash partition table
dependencies.lock      Locked ESP-IDF component dependencies
```

The primary firmware files are:

- [`main/main.cpp`](main/main.cpp): boot behavior, button handling, sensor
  sampling, averaging, and the main loop
- [`main/sensor.cpp`](main/sensor.cpp): BMV080 initialization and reading
  validation
- [`main/screen.cpp`](main/screen.cpp): e-paper initialization, measurement
  display, and status screens
- [`main/pm_link.cpp`](main/pm_link.cpp): BLE pairing, reconnection, timing,
  and PM-reading transfer
- [`main/pm_protocol.h`](main/pm_protocol.h): BLE data structures shared with
  the companion AQM project
