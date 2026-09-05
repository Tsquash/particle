# PM-to-AQM Integration Plan

## Purpose

This document is the implementation handoff for integrating the battery-powered `matter-PM` ESP32-C6 with the Matter-over-Thread `matter-AQM` ESP32-C6.

The implementation must support three persistent PM operating modes:

1. **Standalone** — sample PM once per minute, update the PM EPD, and deep-sleep. Do not initialize a radio.
2. **AQM link** — perform the same local sampling/display cycle, send PM1/PM2.5/PM10 to one paired AQM, receive timing from it, and deep-sleep.
3. **Matter** — reserve a distinct selectable mode for a future standalone Matter endpoint. In this project it displays a short “Matter mode unavailable” message and then uses the standalone sampling/display/sleep behavior, without initializing BLE or Matter.

The AQM must publish the linked PM values through three standard Matter concentration clusters on its existing air-quality endpoint. Its normal EPD pages must not change.

## Repositories and accepted baseline

- PM: `/Users/caleblightfoot/Documents/PlatformIO/Projects/matter-PM`
- AQM: `/Users/caleblightfoot/Documents/PlatformIO/Projects/matter-AQM`
- Treat all existing uncommitted AQM changes as the intended baseline. Preserve them and do not rewrite unrelated code.
- Both current projects build successfully before the integration work.
- The PM directory is not currently a Git repository; do not assume Git-based rollback is available there.

### Relevant current code

PM:

- `main/main.cpp` initializes the EPD and BMV080 and currently polls every five seconds.
- `main/sensor.cpp` starts the BMV080 in continuous mode and returns float-typed values whose useful values are integers.
- `main/screen.cpp` owns the 122×250 EPD drawing and current obstruction behavior.
- `main/spi_pins.h` defines the shared BMV080/EPD SPI wiring.
- PM buttons are active-low GPIO0 and GPIO1; enable pull-ups.

AQM:

- `main/main.cpp` creates the Matter node/endpoint and handles the current boot factory-reset action.
- `main/buttons.cpp` / `buttons.h` use GPIO1 for normal page changes and GPIO0 for the boot factory-reset hold.
- `main/sense.cpp` schedules the sensor cycle at second `:50` and the display refresh at the minute boundary. The blocking CO2 read takes roughly ten seconds.
- `main/matter_air_quality_sensor.cpp` creates the Air Quality endpoint plus temperature, humidity, CO2, TVOC, and NO2 concentration clusters.
- `main/screen.cpp` / `screen.h` own the AQM EPD UI.

### Thread build correction already made

The tracked generated `matter-AQM/sdkconfig.esp32c6` was overriding the Thread defaults with an old Wi-Fi configuration. This plan's research pass already made the following narrowly scoped correction:

- `/sdkconfig.esp32c6` is ignored and removed from source control so PlatformIO regenerates it from `sdkconfig.defaults`.
- `main/main.cpp` now has a compile-time error unless both `CONFIG_OPENTHREAD_ENABLED` and `CONFIG_ENABLE_MATTER_OVER_THREAD` are set.
- The misleading Wi-Fi transport comment in `main/main.cpp` now says Thread.

Keep this correction. `sdkconfig.defaults` is the canonical PlatformIO defaults file; `sdkconfig.defaults.c6_thread` may remain as the explicit reference profile. Keep `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING` disabled so application BLE remains available after Matter commissioning.

## Scope

### In scope

- PM mode selection and persistence.
- Once-per-minute BMV080 burst sampling and deep sleep.
- Local PM EPD behavior in all three modes.
- Secure one-to-one PM/AQM pairing, unlinking, and persistent identity.
- BLE GATT transfer of integer PM1, PM2.5, and PM10 values.
- AQM-to-PM timing acknowledgements.
- AQM remote-PM cache, freshness handling, and diagnostics.
- PM1, PM2.5, and PM10 Matter clusters on the existing AQM endpoint.
- PM contribution to the AQM aggregate Matter Air Quality value.
- Factory-reset interactions and temporary pairing/unlink screens.
- Unit, integration, hardware, timing, and power verification.

### Out of scope

- Implementing the PM's future standalone Matter endpoint.
- Any permanent PM values, link status, or delivery status on normal AQM EPD pages.
- Any PM delivery or “AQM unavailable” indicator on the normal PM reading screen.
- Wi-Fi credentials, Wi-Fi/NTP on the PM, ESP-NOW, or joining the PM to Thread.
- Fixing unrelated AQM behavior or sensor issues.
- Solving the powered-sensor hardware limitation with a load switch; the BMV080 is permanently powered on the current PCB.

## Architecture decision

Use **Bluetooth Low Energy GATT**, with the AQM as an always-available peripheral/server and the PM as a short-lived central/client.

```text
PM ESP32-C6                                      AQM ESP32-C6
------------                                    -----------------
wake near :42                                   always awake
BMV080 burst                                    Matter over Thread
average valid integer samples   BLE GATT        custom PM service
PM1 / PM2.5 / PM10  ----------> validate/cache
                              <---------- ACK + next wake delay
update PM EPD                                    at :50 start local reads
deep-sleep                                       after CO2 read, publish
                                                 local + fresh cached PM
```

This is the best match for the constraints:

- The PM needs only a brief nearby connection and does not need network credentials, NTP, or a Thread attach each minute.
- The AQM is already a Matter-over-Thread device. BLE and IEEE 802.15.4 coexistence is supported on ESP32-C6, though radio operations are time-sliced.
- Keeping BLE after Matter commissioning is an explicitly supported ESP-Matter configuration when `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING` is disabled.
- ESP-NOW would require enabling the Wi-Fi radio/subsystem alongside Thread and would add channel, security, and power complexity.
- Making the PM a Thread sleepy end device now would expand scope into Thread provisioning and attachment. Deep-sleep/reattach every minute is also a poor fit compared with a short BLE transfer.

Do not initialize a second Bluetooth controller or NimBLE host in the AQM. Register the application GATT service with the same ESP-IDF NimBLE host used by the Matter platform.

## Required preliminary compatibility spike

Before implementing the full UI and sampling flow, prove application BLE and Matter-over-Thread can coexist in this exact Arduino + ESP-IDF + ESP-Matter build:

1. Confirm the regenerated AQM config contains:
   - `CONFIG_OPENTHREAD_ENABLED=y`
   - `CONFIG_ENABLE_MATTER_OVER_THREAD=y`
   - `CONFIG_BT_ENABLED=y`
   - `CONFIG_BT_NIMBLE_ENABLED=y`
   - `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING` unset/disabled
2. Add a temporary custom 128-bit GATT service using ESP-IDF NimBLE C APIs, without calling a second `NimBLEDevice::init()` or starting another host task.
3. Commission the AQM over Thread, reboot it, and verify the custom service remains advertisable and connectable after commissioning.
4. Verify a BLE connect/write/disconnect does not detach Thread and a Matter read still succeeds during repeated BLE connects.
5. Verify reopening a Matter commissioning window does not corrupt the custom service. Application advertising may pause while Matter commissioning is active and resume afterward.
6. Record the correct lifecycle hook used to start/restart application advertising after NimBLE host synchronization. Remove any throwaway code and build the real module on that result.

If this exact framework version exposes a lifecycle conflict, stop at the spike and document the failing event/configuration before redesigning. Do not silently introduce Wi-Fi or a second BLE stack.

## PM runtime design

### Persistent modes

Define an explicit stable enum; never use booleans whose meaning could change later:

```text
0 = STANDALONE
1 = AQM_LINK
2 = MATTER_PLACEHOLDER
```

Store the selected mode in NVS. If the key is absent or invalid, default to `STANDALONE`. Selecting standalone or Matter mode must not erase the saved AQM association. This lets a user temporarily run standalone and return to the existing link later.

### Boot and wake classification

Use `esp_sleep_get_wakeup_cause()` to distinguish cold boot, timer wake, and button wake.

On every boot/wake:

1. Configure GPIO0 and GPIO1 as active-low inputs with pull-ups before interpreting them.
2. If either button caused the wake, wait a short debounce/chord window (recommended 250–400 ms) to allow the second button to be pressed.
3. If both buttons are held stably, enter mode selection.
4. If only one is held, perform no action, wait for release, and return to the current mode's sleep schedule. Do not take a sensor reading solely because of an accidental single-button wake.
5. On a timer wake with no chord, run the selected mode.
6. On cold boot with no chord, run the saved mode. Show the startup art only when appropriate; do not spend three seconds on it on every timer wake.

GPIO0–GPIO7 on ESP32-C6 support the applicable external deep-sleep wake mechanism. Configure GPIO0 and GPIO1 for “any low” wake plus the timer wake. Validate that the selected sleep API and internal pull-ups remain effective during deep sleep on the real board. If internal pulls are not retained reliably, document the need for external pull-ups rather than allowing floating wake inputs.

Before entering deep sleep, require both buttons to be released so a held-low input cannot create a rapid wake loop.

### Mode-selection UI

Holding both PM buttons during cold boot or any awake portion returns to mode selection as though the device were newly configured.

- Display all three named modes, one selected at a time.
- GPIO0 short press: next mode.
- GPIO1 short press: select/confirm.
- Use release-then-press semantics so the boot chord cannot also confirm a mode.
- Persist only after confirmation.
- If `MATTER_PLACEHOLDER` is chosen, briefly display “Matter mode unavailable,” then enter the standalone runtime path while retaining mode value `2`.
- If `AQM_LINK` is chosen and a valid AQM association already exists, use it. Otherwise begin AQM discovery/pairing.

### Sensor lifecycle and averaging

Do not use BMV080 duty-cycling mode as a substitute for ESP deep sleep. The Bosch duty-cycling API still requires the service routine at least once per second, so it is incompatible with an uninterrupted ~50-second MCU deep sleep.

For each scheduled measurement:

1. Initialize/open the BMV080.
2. Start continuous measurement for a short burst.
3. Service the driver at its required cadence.
4. Collect approximately four valid readings over about five seconds. Bosch documents roughly 1.9 seconds to the first continuous result and about 1.03 seconds for subsequent results, so use observed result-ready events rather than fixed assumptions alone.
5. Treat all PM values as whole-number µg/m³. Convert each accepted result to an integer once; accumulate with at least 32-bit sums; round the final arithmetic mean to the nearest integer.
6. Exclude an entire sample triplet if the sensor marks it obstructed, any component is negative/not finite, or any component is outside the supported 0–1000 range. Do not average individual components from a rejected triplet.
7. If at least one valid triplet exists, average each PM size independently using the same accepted sample count.
8. Stop measurement and close the sensor before sleeping. Put SPI chip selects and other sensor-facing GPIOs into verified low-leakage states.

The implementation should use result-ready callbacks/state, not assume that every service call produces a new reading.

### Obstruction and failed-minute behavior

- If some samples are obstructed but at least one sample is valid, average only the valid samples, display that average, and send it with `ANY_OBSTRUCTION` set for diagnostics.
- If every sample in the burst is obstructed or otherwise invalid, retain the previous valid values on the PM EPD, matching the current display intent.
- In AQM-link mode, send an **invalid-status heartbeat** with no usable PM values. This lets the AQM acknowledge timing while not refreshing PM data freshness. It satisfies the requirement to send no fresh PM data for that minute.
- Do not turn an invalid minute into a repeat of the previous values on the wire.
- Keep the current local obstruction messaging behavior as closely as practical, but do not stay awake indefinitely waiting for an obstruction to clear. Finish the bounded burst, render the appropriate local state, and sleep.

### Order within an AQM-link wake

Prioritize delivery before the slower EPD refresh:

1. Classify wake/buttons.
2. Sample/average the BMV080.
3. Stop/close the BMV080.
4. Connect and transmit to the AQM, with bounded retries.
5. Update/hibernate the EPD.
6. Configure timer/button wake and deep-sleep.

BLE discovery may run asynchronously during sensor warm-up if it does not disturb the BMV080 service cadence. Implement the simpler sequential version first and optimize only after measuring the awake-time budget.

### PM retained and persistent state

Use NVS only for infrequently changing configuration:

- Namespace `pmcfg` (or another documented stable name).
- Schema version.
- Mode enum.
- Whether an application association is committed.
- AQM bonded identity reference/address type and display short ID.
- Negotiated protocol version/capabilities.

Let NimBLE's NVS bond store retain BLE keys. Do not duplicate or log keys.

Use RTC retained memory, with a magic/version/check value, for state that changes every wake:

- Previous valid PM averages for local display fallback.
- EPD partial/full-refresh counter if needed.
- Last cycle start/fallback cadence state.
- Diagnostic consecutive delivery failures if desired.

Do not write sequence numbers, readings, retry counts, or minute timing to flash every minute.

## AQM boot buttons, pairing, and unlinking

### Boot intent precedence

The existing AQM controls are:

- GPIO0 / `BTN2`: boot hold currently factory-resets/decommissions Matter.
- GPIO1 / `BTN1`: normal page-change button.

Refactor boot handling so the two-button chord is resolved before the one-button factory-reset action:

```text
both GPIO0 + GPIO1 held stably at boot -> PM link management
GPIO0 held alone at boot              -> existing Matter factory reset
all other cases                       -> normal boot
```

Important implementation details:

- Configure both inputs with pull-ups and poll the boot chord before enabling the GPIO1 page-change interrupt, or suppress that interrupt until boot intent is resolved.
- Require a stable hold of about one second, then require release before accepting a confirmation click.
- Do not let “both held” fall through to the GPIO0-only Matter reset path.
- A GPIO0-only factory reset must retain its current user-visible behavior, while also clearing the PM application association and its BLE bond.

### Preconditions

The AQM must already be commissioned to Matter-over-Thread before accepting a PM link. If the user enters PM link management on an uncommissioned AQM:

1. Show temporary text such as `Set up Matter first`.
2. Do not open PM pairing or unlink state.
3. Continue into the existing Matter commissioning screen/flow.

### No linked PM

When both AQM buttons are held at boot and no PM is linked:

1. Initialize the EPD as needed and show a temporary `Searching for PM` screen.
2. Open a 60-second application pairing window.
3. Advertise the custom PM-link service with its pairable flag set.
4. The chord itself is the AQM owner's authorization; require no later AQM click during pairing.
5. On success, show a temporary confirmation containing the PM short ID, then restore the normal AQM display flow.
6. On timeout/failure, show simple failure text briefly and restore normal operation.

### Existing linked PM

When both buttons are held and a PM is already linked:

1. Show `PM linked: <short-id>` and `GPIO1: unlink / GPIO0: cancel` (simple text is explicitly acceptable until graphics exist).
2. Wait for the original chord to be released.
3. A fresh GPIO1 click removes the application association and that peer's NimBLE bond, clears the cached PM values, sets all three Matter PM measured values to null, and excludes PM from aggregate air quality.
4. GPIO0 or timeout cancels without changing the link.
5. Show a short result screen, then return to normal UI.

Do not expose this unlink operation on normal AQM pages.

## BLE pairing and identity

### Roles and advertising

- AQM: BLE peripheral/GATT server.
- PM: BLE central/GATT client, enabled only in `AQM_LINK` mode or while explicitly pairing into that mode.
- AQM normally advertises the service so its bonded PM can reconnect quickly. Use a reasonably fast interval (start around 100–250 ms and tune from measurements); AQM power is not the constrained side.
- Pairing-window advertisements include protocol version, a non-secret six-character short ID, and a pairable flag.
- Normal advertisements include enough service/identity information for the linked PM to find the AQM, but have the pairable flag clear.

Generate one custom 128-bit service UUID and fixed characteristic UUIDs, place them in the shared protocol definition, and never reuse Matter service UUIDs.

### Discovery with multiple AQMs

During PM pairing:

1. Scan only for advertisements containing the custom service UUID and pairable flag.
2. Deduplicate by BLE identity and keep candidates ordered by RSSI.
3. Show one candidate short ID at a time on the PM EPD.
4. GPIO0 short press advances to the next candidate.
5. GPIO1 selects/connects to the displayed candidate.
6. A long GPIO0 press cancels pairing.
7. The entire operation times out after 60 seconds.

The short ID is only a selection aid; it is not authentication. Derive it consistently from the public identity (for example, the final three bytes rendered as hex) or from a persisted random public identifier.

### Authentication

Require BLE LE Secure Connections, bonding, and MITM protection:

- Configure the appropriate Display/Yes-No I/O capability on both devices.
- Let the BLE stack generate the six-digit Numeric Comparison value.
- Show the same six-digit value on both EPDs.
- AQM automatically accepts it only because the device is inside the boot-authorized pairing window.
- PM GPIO1 confirms; PM GPIO0 cancels.
- Commit the application association only after authenticated bonding, capability/version validation, and a successful link-commit exchange.
- Outside the pairing window, reject security establishment by unknown peers.
- Normal report/ACK/control characteristics require an encrypted, authenticated connection and a peer identity matching the committed association.

One AQM links to one PM and one PM links to one AQM. If pairing would replace an existing PM-side AQM association, show both old/new short IDs and require an explicit PM-side confirmation before deleting the old bond. The AQM's existing-link boot path goes to unlink, not silent replacement.

### Bond and reset semantics

- Save the application association in NVS separately from the NimBLE bond store so an authenticated but uncommitted bond is never treated as linked.
- On canceled/failed pairing, delete any newly created uncommitted bond.
- AQM separate unlink deletes only the PM association/bond and PM cache; it does not decommission Matter.
- AQM Matter factory reset also deletes the PM association/bond/cache.
- PM mode changes do not erase its association.
- If NVS says linked but the corresponding bond is missing/corrupt, fail closed, mark unlinked, and require pairing again.

## GATT service and wire protocol

### Characteristics

Use three custom characteristics:

1. **Capabilities/Control** — authenticated read/write for protocol negotiation and link commit.
2. **Measurement Report** — authenticated write-with-response from PM to AQM.
3. **Application ACK** — authenticated indication from AQM to PM.

Use an indication for the application ACK so the AQM knows it reached the client. Still retain the report ID and application status; the ATT write response alone does not mean the AQM accepted the sample.

GATT/NimBLE callbacks must only validate bounded input, copy it to a fixed-size queue, and return. A dedicated AQM PM-link task performs association checks, cache mutation, ACK construction, and diagnostics. Never call Matter attribute APIs or perform EPD/I2C work in a BLE callback.

### Serialization rules

- Protocol version starts at `1`.
- Serialize explicitly into byte arrays; do not transmit C/C++ structs or compiler padding.
- All multibyte integers use little-endian order.
- PM units are integer µg/m³.
- Reject unknown versions/types, wrong lengths, bad CRCs, invalid flags, and values outside 0–1000.
- Use CRC-16/CCITT-FALSE over all bytes preceding the CRC. BLE already provides link integrity; this CRC primarily catches schema/serialization mistakes and gives clear diagnostics.
- Log protocol version, message type, report ID, status, timing, RSSI, and retry count. Never log keys or full bond material.

### Version 1 measurement frame (20 bytes)

| Offset | Size | Field | Definition |
| ---: | ---: | --- | --- |
| 0 | 2 | magic | Literal bytes `P`, `M` |
| 2 | 1 | version | `1` |
| 3 | 1 | type | `1 = MEASUREMENT` |
| 4 | 4 | report_id | Random 32-bit ID generated for this wake/report |
| 8 | 2 | pm1 | Integer µg/m³; zero and ignored when data-valid is clear |
| 10 | 2 | pm25 | Integer µg/m³; zero and ignored when data-valid is clear |
| 12 | 2 | pm10 | Integer µg/m³; zero and ignored when data-valid is clear |
| 14 | 1 | valid_sample_count | Number of triplets included in the average |
| 15 | 1 | flags | Bit field below |
| 16 | 2 | acquisition_ms | Saturating sensor-burst duration for diagnostics |
| 18 | 2 | crc16 | CRC over bytes 0–17 |

Flags:

- bit 0: `DATA_VALID`
- bit 1: `ANY_OBSTRUCTION`
- bit 2: `ALL_SAMPLES_OBSTRUCTED`
- bit 3: `SENSOR_ERROR`
- bit 4: `OUT_OF_RANGE_SAMPLE`
- bits 5–7: reserved, transmit zero

For a wholly invalid minute, clear `DATA_VALID`, set the applicable reason flags, set `valid_sample_count=0`, and transmit zero PM fields. The AQM acknowledges and resynchronizes the PM but must not refresh the last-valid timestamp.

Generate a new random `report_id` with `esp_random()` for each logical report and reuse it for all retries of that report. The AQM keeps a small recent-ID set per bonded peer so a retry is acknowledged but not applied twice. This avoids an NVS write every minute and remains safe across deep-sleep resets.

### Version 1 ACK frame (16 bytes)

| Offset | Size | Field | Definition |
| ---: | ---: | --- | --- |
| 0 | 2 | magic | Literal bytes `P`, `M` |
| 2 | 1 | version | `1` |
| 3 | 1 | type | `2 = ACK` |
| 4 | 4 | report_id | Echoes the report |
| 8 | 1 | status | Application status enum |
| 9 | 1 | aqm_second | AQM second 0–59 for logs; `0xFF` if timing invalid |
| 10 | 4 | next_wake_delay_ms | Delay from ACK creation to next target burst; zero if invalid |
| 14 | 2 | crc16 | CRC over bytes 0–13 |

ACK status values:

```text
0 = ACCEPTED_VALID
1 = ACCEPTED_INVALID_STATUS
2 = DUPLICATE
3 = BAD_FRAME
4 = UNSUPPORTED_VERSION
5 = NOT_LINKED
6 = BUSY_RETRY
7 = TIMING_INVALID
```

`ACCEPTED_VALID`, `ACCEPTED_INVALID_STATUS`, and `DUPLICATE` may carry valid next-wake timing. Other errors do not alter PM association or fallback cadence.

Define similarly explicit, CRC-protected control request/response payloads (maximum 20 bytes) for `GET_CAPABILITIES` and `LINK_COMMIT`. Capabilities must include supported protocol version range and PM1/PM2.5/PM10 support. Do not overload PM value fields for pairing control.

## Timing and power schedule

### Target phase

The PM measurement burst should begin near AQM wall-clock second `:42`:

```text
:42       PM wakes and starts BMV080 burst
:47–:49   PM sends valid average or invalid-status heartbeat
:50       AQM begins its local DHT/CO2/VOC cycle
~:59–:00  AQM snapshots PM cache and updates Matter attributes
:00       AQM refreshes its normal EPD
```

The goal is a valid PM packet in the AQM cache before `:50`. The AQM must never wait for it.

### AQM time source for ACKs

The PM does not use Wi-Fi or NTP. The AQM's external RTC/maintained time is the authority.

Avoid reading the shared I2C RTC directly from a BLE callback. Maintain a small AQM timing service:

- Capture an RTC wall-clock reference plus `esp_timer_get_time()` during normal startup/minute processing under the existing I2C ownership rules.
- Extrapolate the current second from monotonic time when constructing an ACK.
- Periodically resynchronize the reference when the AQM updates/reads its RTC.
- Compute `next_wake_delay_ms` to the next occurrence of second `:42`, measured from ACK creation.
- If the RTC/reference is invalid, send `TIMING_INVALID` and zero delay.

### PM sleep calculation

- On a successful ACK with valid timing, use `next_wake_delay_ms`, minus only measured shutdown preparation if necessary, as the deep-sleep timer. Re-synchronize on every successful transfer so internal RTC drift does not accumulate.
- On no ACK or invalid timing, preserve a 60-second **start-to-start** cadence: `sleep_ms = max(minimum_sleep, 60000 - elapsed_since_cycle_start)`. Do not simply sleep 60 seconds after all work, which would drift by the awake duration every cycle.
- Clamp nonsensical ACK delays to a safe range before using them. A suggested valid range is 20–80 seconds for steady operation; a one-time pairing response may explicitly schedule a longer first delay.
- Use up to three quick connection/report attempts within one bounded total radio budget of roughly 3–5 seconds. Reuse the same report ID. Do not remain awake indefinitely when the AQM is unavailable.
- After the retry budget, update the local EPD normally and sleep on fallback cadence. Do not show delivery failure on the PM display.

Measure real wake-to-sleep duration, scan/connect latency, BMV080 duration, EPD duration, and current draw. Tune `:42` only from those measurements while retaining at least a one-second margin before the AQM's `:50` work starts.

### Deep-sleep preparation

Before `esp_deep_sleep_start()`:

- Stop/close the BMV080 and verify it enters its lowest available state despite remaining powered.
- Disconnect BLE and stop PM scanning/client activity.
- Hibernate the EPD.
- Set SPI chip selects inactive and test for back-power/leakage through GPIOs.
- Configure timer wake plus either-button active-low wake.
- Flush only essential serial output; do not add a long unconditional delay.

## AQM remote-PM cache

Create a module independent of BLE and Matter that owns the latest accepted remote PM state. Suggested fields:

```text
pm1, pm25, pm10                  uint16_t
has_valid_sample                bool
last_valid_received_us          monotonic timestamp
last_report_id                  uint32_t
last_valid_sample_count         uint8_t
last_flags                      uint8_t
last_rssi                       int8_t
invalid_heartbeat_count         counter
delivery/parse/auth counters    diagnostics
```

Use a mutex or single-owner queue/task. BLE writes and Matter reads must receive coherent triplets—never a mixture of two reports.

A valid frame from the committed bonded peer atomically replaces all three values and refreshes `last_valid_received_us`. An invalid-status heartbeat updates diagnostics and timing only. A duplicate is acknowledged but does not rewrite state.

### Freshness

- Freshness limit: three missed one-minute cycles, implemented as an elapsed monotonic limit of 180 seconds from the last accepted valid frame.
- Before the first valid linked sample and after the freshness limit, all three PM Matter `MeasuredValue` attributes are null/unavailable.
- When stale, exclude PM from aggregate Air Quality.
- A later valid packet restores all three values and their contribution together.
- Disconnects, scan misses, and invalid-status heartbeats never block the AQM sensor/Matter cycle.

Use elapsed time as the source of truth rather than incrementing a miss counter only at an assumed callback boundary. Counters can still be logged for debugging.

## Matter model changes on the AQM

### Clusters

On the existing Air Quality Sensor endpoint, add server clusters:

- PM1 Concentration Measurement
- PM2.5 Concentration Measurement
- PM10 Concentration Measurement

For each cluster:

- Add the Numeric Measurement feature using the esp-matter feature API.
- Set `MeasurementUnit` with the generated enum `MeasurementUnitEnum::kUgm3` (`0x04`), not a magic number in application code.
- Configure nullable `MeasuredValue`, `MinMeasuredValue=0`, and `MaxMeasuredValue=1000` as floats because the Matter schema uses nullable float, while converting only from integer PM readings.
- Initialize `MeasuredValue` to null.
- Retain pointers/IDs for efficient updates in `MatterAirQualitySensor`.
- Update all three from one remote-cache snapshot during the existing minute sensor publication path.

The local esp-matter checkout already includes the three cluster namespaces and numeric feature helpers. For null use `esp_matter_nullable_float(nullable<float>())`; for a value use the equivalent non-null `nullable<float>`/helper supported by the checked-in API. Check every cluster/feature/attribute creation and update return value.

Adding clusters changes the endpoint data model, so existing commissioned test devices may need to be recommissioned. Recommissioning is acceptable for this project.

### Aggregate Air Quality

Keep the current “worst valid pollutant wins” approach. Add PM2.5 and PM10; publish PM1 but do not use it in the aggregate because there is no accepted PM1 indoor AQ category standard.

Use these integer thresholds as an instantaneous indoor-device heuristic:

| Matter category | PM2.5 µg/m³ | PM10 µg/m³ |
| --- | ---: | ---: |
| Good | 0–9 | 0–54 |
| Fair | 10–35 | 55–154 |
| Moderate | 36–55 | 155–254 |
| Poor | 56–125 | 255–354 |
| Very Poor | 126–225 | 355–424 |
| Extremely Poor | ≥226 | ≥425 |

Map these rows to the existing Matter AirQuality enum values. The AQM aggregate is the worst category among its valid CO2, TVOC, NO2, PM2.5, and PM10 contributors. If remote PM is absent/stale, the local contributors determine the result.

These are not a regulatory AQI calculation. EPA particulate AQI breakpoints are defined around longer averaging/reporting conventions, while this product publishes a short burst once per minute. Name the code accordingly (for example, `classifyInstantaneousPm25`) and document the distinction.

### AQM publication ordering

The current AQM starts local sensing at second `:50`, including the blocking CO2 read. BLE reception must continue in its own task during that work. Immediately before the existing Matter update call:

1. Take one coherent remote-PM cache snapshot.
2. Determine freshness using monotonic elapsed time.
3. Update all local attributes as today.
4. Update all three PM attributes to values or all three to null.
5. Compute/update aggregate Air Quality from the same local and PM snapshot.

Never delay this path waiting for a PM connection or packet.

## AQM UI constraints

Do not alter normal AQM pages, page rotation, layout, or add PM readings/link icons.

Only add temporary, blocking boot-management screens for:

- `Set up Matter first`
- `Searching for PM`
- pairing candidate/SAS code as needed
- pairing success/failure
- linked PM unlink confirmation
- unlink success/cancel

Implement these as simple text drawing functions in `screen.cpp`/`screen.h` so graphics can be substituted later. Restore the existing normal startup/current page afterward.

## Recommended module/file changes

Names may be adjusted to match project style, but keep responsibilities separated.

### `matter-PM`

- `main/main.cpp`
  - Replace the perpetual five-second loop with wake classification and a one-cycle state machine.
  - Dispatch by the three-value mode enum.
  - Guarantee all normal paths terminate in deep sleep.
- `main/sensor.cpp` / `sensor.h`
  - Expose bounded `beginBurst`, service/result collection, averaging status, `stop`, and `close` behavior.
  - Return an integer triplet plus sample count/status flags rather than float display values.
- `main/buttons.cpp` / `buttons.h` (new)
  - Pull-ups, debounce, chord detection, menu presses, release gating, and wake configuration.
- `main/pm_config.cpp` / `pm_config.h` (new)
  - Versioned NVS mode and association storage.
- `main/pm_protocol.cpp` / `pm_protocol.h` (new)
  - Byte serialization/parsing, CRC, constants, enums, validation, and test vectors.
- `main/pm_link_client.cpp` / `pm_link_client.h` (new)
  - NimBLE central, scan/candidate selection, secure pairing, report/ACK exchange, retry budget, and disconnect.
- `main/wake_scheduler.cpp` / `wake_scheduler.h` (new)
  - ACK timing validation, fallback start-to-start cadence, RTC retained state, and deep-sleep entry.
- `main/screen.cpp` / `screen.h`
  - Mode menu, Matter-placeholder text, candidate selection, SAS confirmation, and pair result screens.
- `sdkconfig.defaults` / PlatformIO config
  - Enable only the NimBLE central/security/NVS features needed for AQM-link mode. Do not enable Wi-Fi or OpenThread for this scope.

### `matter-AQM`

- `main/main.cpp`
  - Keep the Thread compile-time guard.
  - Add boot-intent routing, PM-link task startup, pairing/unlink flow, reset cleanup, and nonblocking cache snapshot use.
- `main/buttons.cpp` / `buttons.h`
  - Add explicit boot chord classification and defer/suppress the page ISR until resolved.
- `main/pm_protocol.cpp` / `pm_protocol.h` (new)
  - Exact protocol implementation matching PM, including compile-time frame-size assertions and identical test vectors.
- `main/pm_link_server.cpp` / `pm_link_server.h` (new)
  - Custom GATT registration on the existing NimBLE host, advertising lifecycle, security callbacks, pairing window, peer authorization, queues, and ACKs.
- `main/remote_pm_cache.cpp` / `remote_pm_cache.h` (new)
  - Atomic triplet storage, deduplication, diagnostics, freshness, and reset.
- `main/pm_link_store.cpp` / `pm_link_store.h` (new, or combine with server if still cohesive)
  - Versioned NVS application association and bond consistency/cleanup.
- `main/aqm_timebase.cpp` / `aqm_timebase.h` (new, or a small addition to `sense`)
  - RTC-to-monotonic reference and next-`:42` delay calculation without BLE-callback I2C.
- `main/matter_air_quality_sensor.cpp` / `.h`
  - Three PM clusters, nullable update path, PM classification, and aggregate inclusion.
- `main/screen.cpp` / `.h`
  - Temporary pairing/unlink text screens only.
- `.gitignore`, `sdkconfig.defaults`, `sdkconfig.defaults.c6_thread`
  - Preserve the generated-config correction and required Thread/BLE settings.

The protocol files may be generated/copied from one canonical small source, but each firmware must build independently. Put a prominent version/source comment in both copies and add a test that their constants and golden byte vectors remain identical.

## Logging and diagnostics

Use stable prefixes such as `[PM-SAMPLE]`, `[PM-LINK]`, `[PM-SLEEP]`, `[AQM-PMLINK]`, and `[AQM-PMCACHE]`.

At minimum log:

- Wake cause, selected mode, cycle elapsed time, and chosen sleep delay.
- BMV080 startup/result/stop errors, valid sample count, excluded sample count, and obstruction flags.
- BLE state transitions, candidate short ID, RSSI, connection duration, security state, report ID, retry, ACK status, and protocol version.
- Pairing-window entry/timeout/commit/unlink and reset cleanup.
- Cache acceptance/duplicate/rejection, age at Matter publication, transitions to stale/null, and restoration.
- Matter cluster creation/update failures and aggregate category changes.

Do not print long-term keys, IRKs/LTKs, complete NVS blobs, or secret stack state.

## Implementation phases

### Phase 0 — Preserve baseline and prove coexistence

1. Capture current file status in both projects and do not overwrite existing AQM changes.
2. Complete the Thread build correction verification from a regenerated config.
3. Run the application-BLE + Matter-over-Thread compatibility spike.
4. Establish protocol golden vectors and shared constants.

Exit criterion: commissioned AQM remains on Thread while a test client connects to the custom GATT service and exchanges an authenticated test value.

### Phase 1 — PM one-cycle power architecture

1. Add mode/NVS/button/wake modules.
2. Refactor BMV080 into a bounded continuous-mode burst.
3. Implement valid-sample averaging and all-invalid fallback.
4. Update/hibernate the EPD and deep-sleep with timer + buttons.
5. Validate standalone and Matter-placeholder modes before adding BLE.

Exit criterion: both modes run one bounded cycle per minute, both buttons reopen mode selection, a single-button wake returns to sleep, and current is measured in deep sleep.

### Phase 2 — Protocol, BLE transport, and secure pairing

1. Implement/test serializers, parser, CRC, flags, deduplication, and ACK timing math.
2. Implement AQM GATT server/advertising and PM client/retry path.
3. Implement boot pairing window, multi-candidate PM UI, Numeric Comparison, association commit, and normal bonded reconnect.
4. Implement separate unlink and factory-reset cleanup.

Exit criterion: only the physically authorized/bonded PM can update the AQM cache; pairing survives reboot/deep sleep; unlink and Matter reset behave exactly as specified.

### Phase 3 — Timing and cache integration

1. Implement the AQM monotonic timebase and ACK next-wake delay.
2. Target the PM burst at `:42`, add fallback cadence and bounded retries.
3. Integrate the dedicated link task and coherent remote cache.
4. Verify PM arrival before `:50` while AQM local sensors and Thread remain responsive.

Exit criterion: repeated successful ACKs correct drift, outages do not extend awake time beyond the retry budget, and AQM publication never blocks.

### Phase 4 — Matter clusters and air-quality aggregation

1. Add all three numeric concentration clusters to the existing endpoint.
2. Update value/null state from one cache snapshot.
3. Add PM2.5/PM10 instantaneous classifications to worst-of aggregation.
4. Recommission and validate with a general Matter controller plus Apple Home.

Exit criterion: fresh values are readable, three missed cycles produce null, recovery restores them, and local sensor reporting continues throughout.

### Phase 5 — Robustness and power qualification

Run the complete matrix below, fix defects, then document measured awake/sleep current and timing margins.

## Verification matrix

### Automated/native tests

- Measurement and ACK encode/decode golden vectors.
- CRC corruption, truncated/oversized packet, reserved flags, bad version/type, and out-of-range rejection.
- Random report-ID duplicate acceptance without double application.
- Integer averaging with 1–4 valid samples and interleaved obstructions.
- All-obstructed/all-error behavior sends no valid data.
- PM2.5 and PM10 boundary tests for every category edge.
- Worst-of aggregate with fresh PM, stale PM, and no PM.
- Freshness immediately before/at/after 180 seconds.
- Next-`:42` timing calculations around `:41`, `:42`, `:59`, minute rollover, and invalid RTC.
- Fallback sleep subtracts elapsed awake time and clamps safely.
- Mode/NVS schema validation and corrupt/default handling.
- Protocol constants/golden vectors match in both repositories.

### Hardware integration

- Standalone cold boot, timer wake, local reading, EPD update, and deep sleep.
- Matter-placeholder selection remains distinct after reboot but has no radio activity.
- Both-button mode entry during an awake window and from deep sleep.
- Each single button wakes but does nothing and returns to sleep.
- AQM GPIO0-only boot hold still factory-resets Matter.
- AQM two-button boot hold never factory-resets Matter.
- Attempt PM pairing before AQM Matter commissioning.
- Pair one PM/one AQM; cancel at candidate selection and Numeric Comparison.
- Two or more pairable AQMs nearby; select the intended short ID.
- Reboot AQM and deep-sleep/wake PM; bonded reconnect succeeds.
- Unknown/unbonded client outside pairing window cannot write reports.
- PM explicit replacement of an old AQM requires confirmation.
- AQM unlink clears only PM link and nulls values.
- AQM Matter factory reset clears both Matter and PM link.
- PM standalone/Matter mode switch preserves association; return to link reconnects.
- Invalid/obstructed minute leaves PM EPD on previous reading and does not refresh AQM freshness.
- Three missed valid cycles make all three Matter PM attributes null; fourth valid cycle restores all together.
- AQM powered off/out of range: PM finishes retries, updates local display, and sleeps without link indicators.
- Repeated report retry/duplicate does not double count.
- BLE traffic during AQM Thread/Matter report does not detach Thread or corrupt a value triplet.
- Matter commissioning window reopen and application BLE advertising lifecycle.

### Timing/endurance/power

- Log at least 100 consecutive cycles and show PM report accepted before AQM `:50` work with the required margin.
- Run a 24-hour drift/outage/recovery test.
- Measure PM current in sensor acquisition, BLE scan/connect, EPD refresh, and deep sleep.
- Confirm the permanently powered BMV080 actually reaches its documented low-power state after `stop/close`.
- Check for GPIO/SPI back-power and immediate button-wake loops.
- Record average awake duration and battery-life input figures; optimize only after correctness.

### Matter/controller validation

- Read PM1, PM2.5, and PM10 cluster `MeasuredValue`, limits, unit, and feature map with `chip-tool` or an equivalent general Matter client.
- Verify nullable behavior over the stale/recovery test.
- Recommission the changed endpoint and verify Apple Home still operates the AQM.
- Check what Apple Home actually surfaces for each concentration cluster, but do not treat a missing Apple Home tile as proof that the Matter attribute was not published; confirm with the general client.

## Acceptance criteria

The implementation is complete only when all of the following are true:

- PM exposes exactly three persistent modes with standalone default and future Matter placeholder behavior.
- PM normally deep-sleeps for the remainder of a one-minute start-to-start cycle.
- GPIO0/GPIO1 pull-ups, chord menu entry, single-button wake behavior, and release gating work on hardware.
- Each burst averages only valid integer triplets; no obstructed value enters an average.
- A wholly invalid burst does not send a fresh value and does not overwrite the PM's previous local reading.
- BLE link pairing is physically authorized, Numeric-Comparison authenticated, bonded, one-to-one, and persistent.
- AQM GPIO0-only factory reset is preserved; the two-button chord manages PM link without accidental decommissioning.
- Separate unlink and Matter factory reset clear exactly the state specified.
- AQM receives normal linked reports by approximately `:48` and never waits for them at publication time.
- Successful ACKs re-phase PM wake to `:42`; failed ACKs preserve bounded 60-second start-to-start behavior.
- AQM publishes PM1, PM2.5, and PM10 as standard numeric concentration measurements in µg/m³.
- After 180 seconds without a new valid PM report, all three PM values are null and excluded from aggregate Air Quality.
- PM2.5 and PM10 use the approved instantaneous thresholds; PM1 is not an aggregate contributor.
- Normal AQM UI has no PM-related changes, and normal PM UI has no delivery-status changes.
- Matter-over-Thread remains connected during BLE operation and the build cannot silently revert to another Matter transport.
- Both firmware images build cleanly and the automated/hardware tests above pass or have explicitly documented, user-approved exceptions.

## Primary technical references

- Bosch BMV080 datasheet (measurement timing, service cadence, modes, power): [BST-BMV080-DS000](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmv080-ds000.pdf)
- Bosch BMV080 product page: [BMV080 particulate matter sensor](https://www.bosch-sensortec.com/en/products/environmental-sensors/particulate-matter-sensor/bmv080/)
- Espressif application BLE after Matter commissioning: [ESP-Matter FAQ](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/faq.html)
- Espressif ESP32-C6 radio coexistence: [RF coexistence guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/api-guides/coexist.html)
- Espressif ESP32-C6 sleep/wakeup APIs: [Sleep modes](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/sleep_modes.html)
- Espressif ESP-NOW reference, evaluated but not selected: [ESP-NOW API](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/network/esp_now.html)
- EPA PM AQI breakpoint table, used as the basis for category boundaries but not represented as a regulatory minute-by-minute AQI: [AQI breakpoints](https://aqs.epa.gov/aqsweb/documents/codetables/aqi_breakpoints.html)
- WHO ambient/indoor particulate guidance context: [WHO global air quality guidelines](https://www.who.int/publications/i/item/9789240034228)

