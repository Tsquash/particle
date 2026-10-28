# Matter PM Sensor

Matter PM is an ESP32-C6-based particulate-matter sensor. It measures PM1.0,
PM2.5, and PM10, displays the latest measurements locally, and can send them to
a paired Matter AQM (air-quality monitor) over Bluetooth Low Energy (BLE).
The AQM is the Matter-connected device; this firmware acts as its PM sensor
peripheral and does not commission itself as a separate Matter device.

## Hardware

- **Controller:** Seeed Studio XIAO ESP32-C6
- **Particulate sensor:** Bosch BMV080, connected over SPI and driven with the
  SparkFun BMV080 Arduino library
- **Display:** 2.13-inch, 250 x 122 pixel monochrome e-paper display using the
  GDEY0213B74 panel/SSD1680 driver, driven with GxEPD2 and Adafruit GFX
- **Interconnect:** the display and sensor share the ESP32-C6 SPI bus

The SPI pins and chip selects are defined in [`main/spi_pins.h`](main/spi_pins.h):

| Signal | GPIO |
| --- | ---: |
| MOSI | 18 |
| MISO | 20 |
| SCK | 19 |
| BMV080 chip select | 2 |
| E-paper chip select | 16 |
| E-paper reset | 22 |
| E-paper data/command | 23 |

The firmware currently configures the e-paper busy pin as `-1` (not connected).
Check the board schematic and display/sensor hookup documentation when wiring
hardware; the pin assignments above describe the current firmware configuration.

## Operating modes

### Standalone

The PM sensor starts collecting data even when no AQM is available. It polls
the BMV080 for new output approximately every 500 ms and forms one-minute
averages. A valid average requires at least 10 valid samples. Measurements
that are obstructed or outside the valid range are not included in the average.
When all samples in an interval indicate obstruction, the display shows the
obstructed status instead of a numerical reading.

Standalone operation does not require BLE or a network connection. The first
boot makes one automatic attempt to pair with an AQM; after that attempt, the
device can run standalone unless pairing is explicitly requested again with
the pairing button.

### Linked to an AQM

The PM sensor pairs with the companion Matter AQM over BLE. The AQM supplies
the measurement schedule and its RTC time; the PM sensor sends its one-minute
PM averages to the AQM when a BLE connection and notification subscription
are available. The AQM can then use the readings in its Matter air-quality
functionality. The PM sensor continues to take and display local readings if
the AQM is temporarily disconnected.

Pairing is initiated automatically on the first boot. Put the AQM into its
corresponding PM-pairing mode while the PM sensor displays its pairing screen.
The pairing window lasts up to 120 seconds. On success, the peer is saved in
non-volatile storage and the PM sensor will try to reconnect to that AQM on
subsequent boots. If pairing times out, the PM sensor continues in standalone
mode; hold the pairing button during a later boot to try again.

When AQM time is synchronized, the PM display performs a full e-paper refresh
at the top and half past each hour (`:00` and `:30`). Without synchronized
paired time, it falls back to a full refresh every 30 partial refreshes.

## Buttons

Both buttons are active-low and are checked at boot. Hold the button
continuously for at least one second to invoke its action.

| Button | GPIO | Action |
| --- | ---: | --- |
| AQM pair | 0 | Force a new pairing attempt, replacing the previously saved AQM peer |
| Factory reset | 1 | Erase non-volatile storage, including the saved pairing state, and restart |

Factory reset takes priority if both buttons are held. The buttons should be
wired between the GPIO and ground; the firmware enables the internal pull-up.

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
lib/                   Project-local libraries (including GxEPD2 and BMV080)
managed_components/    ESP-IDF managed components
KICAD/                 PCB and schematic design files and local backups
2D/                    Two-dimensional enclosure/cutout drawing
3D/                    Board, component, and enclosure models
UI/                    Display/UI mockups and source artwork
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

## Troubleshooting

- **No sensor readings:** Check sensor SPI wiring and power. The firmware logs
  initialization errors and retries initialization after a failure.
- **Pairing does not complete:** Confirm the AQM is in PM-pairing mode and
  nearby. Hold GPIO0 through boot for at least one second to force another
  pairing attempt.
- **AQM disconnects:** Local sensing and display continue. The saved peer is
  retried on later BLE advertising/reconnection opportunities.
- **Display or sensor bus problems:** The display and BMV080 share the same SPI
  bus; verify chip-select wiring and avoid SPI pin conflicts.
- **Insufficient valid samples:** An interval with fewer than 10 valid samples
  is not published as a PM average. Check sensor status and serial output.

## Companion project

The BLE protocol must stay compatible with the companion Matter AQM firmware.
The shared `PMReading` and `PMTiming` structures and BLE service/characteristic
UUIDs are defined in `main/pm_protocol.h` in both projects.