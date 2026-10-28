#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include "pm_link.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const ble_uuid128_t serviceUUID = PM_SERVICE_UUID;
static const ble_uuid128_t valueUUID = PM_VALUE_UUID;
static ble_addr_t savedPeer{};
static std::atomic<bool> paired{false}, pairing{false}, subscribed{false};
static std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
static uint16_t valueHandle;
static bool enabled = false;
static QueueHandle_t timingQueue;
static uint32_t nextMeasurement = 0;
static std::atomic<uint32_t> timingSyncMillis{0};
static std::atomic<uint8_t> timingMinute{0}, timingSecond{0};

static bool samePeer(const ble_addr_t &a, const ble_addr_t &b) {
    return a.type == b.type && memcmp(a.val, b.val, 6) == 0;
}

static int accessValue(uint16_t conn, uint16_t, ble_gatt_access_ctxt *ctxt, void *) {
    PMTiming timing;
    ble_gap_conn_desc desc;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
        OS_MBUF_PKTLEN(ctxt->om) != sizeof(timing) ||
        ble_hs_mbuf_to_flat(ctxt->om, &timing, sizeof(timing), nullptr) != 0 ||
        (timing.untilMeasurement != UINT32_MAX && timing.untilMeasurement > 60000) ||
        ble_gap_conn_find(conn, &desc) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    if (!pairing && (!paired || !samePeer(desc.peer_id_addr, savedPeer)))
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (pairing) {
        Preferences nvs;
        if (!nvs.begin("pm-link", false)) return BLE_ATT_ERR_UNLIKELY;
        size_t written = nvs.putBytes("peer", &desc.peer_id_addr, sizeof(savedPeer));
        nvs.end();
        if (written != sizeof(savedPeer)) return BLE_ATT_ERR_UNLIKELY;
        savedPeer = desc.peer_id_addr;
        paired = true;
        pairing = false;
        Serial.println("[BLE] AQM link saved in NVS");
    }
    if (timing.untilMeasurement != UINT32_MAX) {
        uint32_t deadline = millis() + timing.untilMeasurement;
        xQueueOverwrite(timingQueue, &deadline);
        timingMinute = timing.minute;
        timingSecond = timing.second;
        timingSyncMillis = millis();
        Serial.printf("[BLE] AQM time %02u:%02u:%02u; next measurement in %lu ms\n",
                      timing.hour, timing.minute, timing.second,
                      (unsigned long)timing.untilMeasurement);
    }
    return 0;
}

static int gapEvent(ble_gap_event *event, void *) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0 ||
                (!pairing && (!paired || !samePeer(desc.peer_id_addr, savedPeer)))) {
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            connection = event->connect.conn_handle;
            Serial.println("[BLE] AQM connected");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        subscribed = false;
        Serial.printf("[BLE] AQM disconnected, reason=%d; keeping local readings\n", event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == valueHandle)
            subscribed = event->subscribe.cur_notify;
        break;
    }
    return 0;
}

static void hostTask(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool initializePMLink(bool forcePair) {
    timingQueue = xQueueCreate(1, sizeof(uint32_t));
    if (!timingQueue) { Serial.println("[BLE] Timing queue allocation failed"); return false; }
    Preferences nvs;
    if (!nvs.begin("pm-link", false)) { Serial.println("[BLE] NVS open failed"); return false; }
    bool attempted = nvs.getBool("attempted", false);
    if (forcePair) nvs.remove("peer");
    paired = nvs.getBytesLength("peer") == sizeof(savedPeer) &&
             nvs.getBytes("peer", &savedPeer, sizeof(savedPeer)) == sizeof(savedPeer);
    pairing = forcePair || (!paired && !attempted);
    if (pairing) nvs.putBool("attempted", true);
    nvs.end();
    // Standalone operation waits for a complete first averaging window. A paired
    // AQM will overwrite this deadline with its next second-40 target.
    nextMeasurement = millis() + 60000;
    if (!paired && !pairing) {
        Serial.println("[BLE] Standalone; radio not started");
        return false;
    }
    if (nimble_port_init() != ESP_OK) { Serial.println("[BLE] Init failed"); return pairing; }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Matter-PM");
    static ble_gatt_chr_def characteristics[2]{};
    characteristics[0].uuid = &valueUUID.u;
    characteristics[0].access_cb = accessValue;
    characteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY;
    characteristics[0].val_handle = &valueHandle;
    static ble_gatt_svc_def services[2]{};
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    services[0].uuid = &serviceUUID.u;
    services[0].characteristics = characteristics;
    int rc = ble_gatts_count_cfg(services);
    if (!rc) rc = ble_gatts_add_svcs(services);
    if (rc) { Serial.printf("[BLE] GATT setup failed rc=%d\n", rc); return pairing; }
    enabled = true;
    nimble_port_freertos_init(hostTask);
    Serial.printf("[BLE] %s\n", pairing ? "Pairing for 120 seconds" : "Reconnecting to saved AQM");
    return pairing;
}

void processPMLink() {
    if (!enabled || !ble_hs_synced() || connection != BLE_HS_CONN_HANDLE_NONE ||
        (!pairing && !paired) || ble_gap_adv_active()) return;
    // Service UUID + pairing flag fit in a legacy advertisement (no scan response).
    uint8_t serviceData[17];
    memcpy(serviceData, serviceUUID.value, 16);
    serviceData[16] = pairing ? 1 : 0;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.svc_data_uuid128 = serviceData;
    fields.svc_data_uuid128_len = sizeof(serviceData);
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 800; // 500 ms; reconnectable without continuous fast advertising
    params.itvl_max = 960;
    int rc = ble_gap_adv_set_fields(&fields);
    if (!rc) rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
    if (rc) Serial.printf("[BLE] Advertising failed rc=%d\n", rc);
}

void finishPMPairing() {
    pairing = false;
    if (!paired && enabled) {
        ble_gap_adv_stop();
        if (connection != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        // No saved partner: stop the host/controller for true standalone operation.
        nimble_port_stop();
        nimble_port_deinit();
        enabled = false;
    }
    Serial.printf("[BLE] Pairing %s\n", paired ? "succeeded" : "timed out; standalone");
}

bool pmLinkPaired() { return paired; }

bool pmLinkFullRefreshDue() {
    const uint32_t sync = timingSyncMillis.load();
    if (!paired || !sync) return false;
    const uint32_t elapsedSeconds = (millis() - sync) / 1000;
    const uint8_t minute = (timingMinute.load() + (timingSecond.load() + elapsedSeconds) / 60) % 60;
    return minute == 0 || minute == 30;
}

bool pmMeasurementDue() {
    uint32_t deadline;
    if (timingQueue && xQueueReceive(timingQueue, &deadline, 0)) nextMeasurement = deadline;
    uint32_t now = millis();
    if ((int32_t)(now - nextMeasurement) < 0) return false;
    nextMeasurement = now + 60000;
    return true;
}

void sendPMReading(const PMReading &reading) {
    if (!paired || !subscribed || connection == BLE_HS_CONN_HANDLE_NONE) return;
    os_mbuf *buffer = ble_hs_mbuf_from_flat(&reading, sizeof(reading));
    int rc = buffer ? ble_gatts_notify_custom(connection, valueHandle, buffer) : BLE_HS_ENOMEM;
    Serial.printf("[BLE] Send PM1=%u PM2.5=%u PM10=%u rc=%d\n", reading.pm1, reading.pm25, reading.pm10, rc);
}
