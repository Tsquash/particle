# PM ↔ AQM link

This replaces the earlier design. No ESP32 sleep, BLE bonding, menus, or extra
Matter mode are implemented.

## Flash and pair

1. Build/upload `matter-AQM` and `matter-PM` separately with PlatformIO's `esp32c6`
   environment. Upload firmware normally to preserve existing NVS.
2. Commission AQM in Apple Home first.
3. Hold **AQM GPIO1** low during boot for one second. Hold until the pairing screen
   appears if uncertain. **AQM GPIO0 remains Matter factory reset**.
4. A PM with fresh NVS automatically enters pairing. Otherwise hold **PM GPIO0**
   low during boot for one second. Only one intended pair should be pairing nearby.
5. Each pairing window lasts 120 seconds. Normal application measurements and
   reporting wait until it finishes. AQM starts the Matter-owned BLE stack first.
   Both screens show success/timeout, wait four seconds, then start normal work.

Entering pairing erases that device's previous peer. Replacing a partner requires
pairing mode on both devices. No confirmation buttons or codes are used.

A fresh PM that times out remembers the attempt; later boots are standalone with
no BLE until GPIO0 is held. Firmware upload without erasing NVS is not a fresh
install. A paired PM that loses AQM keeps displaying locally and advertising for
its saved AQM. AQM scans only while pairing or reconnecting to a saved PM.

## Code map

- `main/pm_link.cpp/.h` in each project: BLE, saved peer, pairing/reconnection.
  AQM also owns the received-reading cache and missed-minute counter here.
- `main/pm_protocol.h`: identical in both projects; keep the two copies in sync.
- PM `main.cpp`: sensor servicing, selecting one fresh sample per minute, EPD.
- AQM `sense.cpp`: one overall-quality calculation shared by screen and Matter.
- AQM `matter_air_quality_sensor.cpp`: standard PM1, PM2.5, PM10 numeric clusters
  on the existing air-quality endpoint, with float values in µg/m³.

## Link and timing

AQM is the central; PM is the peripheral. They use fixed public BLE addresses and
remember one peer in NVS, without encrypted bonding. AQM's `pm-peer` key lives in
`chip-config`, which Matter erases during factory reset (including remote reset).
PM's `pm-link` namespace stores `peer` and the first-pairing `attempted` flag.

One custom GATT characteristic supports:

- **Notify, PM → AQM:** three uint16 values (PM1, PM2.5, PM10), 6 bytes total.
  Only newly selected valid readings are sent; reconnection never replays a cache.
- **Write with response, AQM → PM:** uint32 milliseconds until the next second 40.
  `UINT32_MAX` means a pairing handshake without timing. All fields are little
  endian. AQM subscribes before the handshake. PM saves AQM on the handshake;
  AQM saves PM after its successful response.

AQM sends timing after connection/RTC initialization and roughly every minute.
PM uses `millis()` between corrections and every 60 seconds while standalone.
Measurements target second 40, with a five-second window for a fresh valid output.
Notifications go out before the EPD refresh. AQM retains its existing second-50
sensor alarm and publishes after the existing CO2 read. Matter controls actual
network delivery; this is not a guarantee of a HomePod packet at exactly :55.

The Bosch continuous stream is serviced frequently while awake. It pauses only
around the blocking EPD refresh, then restarts. Failed initialization or a stream
that stops producing output is retried. No deep/light sleep is used. Future sleep
can reuse the timing correction and reconnection path; it will also need to allow
sensor warmup and preserve/restore scheduling across wakeup.

## Validity and quality

Obstructed, outside-range, non-finite, negative, or above-1000 readings are invalid.
The three channels are treated as one sample. Valid floats are rounded once to
integer µg/m³ for display/BLE, then converted to Matter's required float type.
The existing three-digit PM display saturates at 999; BLE/Matter retain 1000.

At each AQM reporting cycle, a fresh sample resets the missed counter. Otherwise:
misses 1, 2, and 3 reuse the previous sample; miss 4 and later publish zeros.
Disconnecting alone does not immediately zero the cache. New valid data restores
values; zero is itself a valid measurement. Rebooted AQM starts with zeros until
it receives fresh data. Measurements are never persisted in NVS.

Overall quality is the worst category across CO2, VOC, NOx, PM1, PM2.5, and PM10.
The screen and Matter use the same calculation. PM thresholds (inclusive bounds):

| Category | PM1 and PM2.5 | PM10 |
|---|---:|---:|
| Good | 0–9 | 0–54 |
| Fair | 10–35 | 55–154 |
| Moderate | 36–55 | 155–254 |
| Poor | 56–125 | 255–354 |
| Very Poor | 126–225 | 355–424 |
| Extremely Poor | 226+ | 425+ |

PM1 reuses the PM2.5 thresholds **by project convention**, approved by the user;
it is not a published PM1 health index. WHO's particulate guidance covers PM2.5
and PM10. These minute samples/categories are not a regulatory exposure AQI.

- [WHO particulate guidelines](https://www.who.int/publications/i/item/9789240034228/)
- [Matter PM cluster definitions, §2.10](https://csa-iot.org/wp-content/uploads/2024/05/matter-application-cluster-specification.pdf)
- [Bosch driver servicing requirements, §5.2.5](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmv080-ds000.pdf)

## Hardware checks after flashing

Builds and host checks cannot validate the radio, EPD, or Apple Home rendering.
Use serial at 115200 on both boards; look for `[BLE]`, `[PM]`, and `[SENSOR]`.

- Pair once: both show success, both log their NVS save, and AQM receives a PM
  triplet. GPIO1 still changes AQM pages after startup.
- Restart each device separately: saved-peer reconnection happens without buttons.
- Unplug AQM: PM continues local readings. Restore AQM: new readings resume.
- Unplug/block PM: AQM logs misses 1/4, 2/4, 3/4 with old values; 4/4 has zeros.
  Unblock/restore PM: the next valid reading restores values and clears misses.
- Check PM logs for measurements near :40–:45 and AQM receipts before its report.
- Let pairing time out on an unpaired PM: subsequent reboot logs standalone and
  does not start BLE. GPIO0 re-enters pairing.
- Factory-reset AQM via GPIO0 (and a controller reset if available): its PM link
  is gone. Recommission Matter and explicitly pair both devices again.
- Check PM values and overall quality in Apple Home. Matter defines all three PM
  clusters; Apple Home may expose only a subset. If Home caches the old endpoint
  layout, remove/recommission the AQM, then pair PM again after commissioning.
