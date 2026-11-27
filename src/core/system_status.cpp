#include "core/system_status.h"
#include <WiFi.h>
#include <esp_timer.h>
#include "IUpsSource.h"
#include "NUTServer.h"
#include "WebApiJson.h"
#include "core/crash_diag.h"
#include "core/device_id.h"
#include "core/memory_stats.h"
#include "core/update_service.h"
#include "core/version.h"
#include "network/mqtt_bridge.h"
#include "network/network_manager.h"

void fillSystemStatus(JsonDocument& doc, const SystemStatusSources& src) {
    doc["version"] = FIRMWARE_VERSION;
    if (src.network) {
        // The MAC of the active link: boards without Wi-Fi have no station MAC
        uint8_t mac[6] = {0};
        src.network->macAddress(mac);
        char dev_id[32];
        snprintf(dev_id, sizeof(dev_id), "%s-%02X%02X%02X", ESP.getChipModel(), mac[3], mac[4], mac[5]);
        doc["device_id"] = String(dev_id);
        doc["hostname"] = src.network->hostname();
    } else {
        doc["device_id"] = getDeviceId();
    }
    doc["mac"] = getMacAddress();
    // esp_timer is 64-bit: millis() wraps after 49.7 days
    doc["uptime_s"] = (uint64_t)(esp_timer_get_time() / 1000000LL);

    // "AP Mode Active", the SSID or "Connecting"; connected and the rest are for other clients
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["status"] = src.network ? src.network->statusText() : String("Disconnected");
    bool wifi_connected = (WiFi.getMode() & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED;
    wifi["connected"] = wifi_connected;
    if (wifi_connected) {
        wifi["ssid"] = WiFi.SSID();
        wifi["ip"] = WiFi.localIP().toString();
        wifi["rssi"] = WiFi.RSSI();
        wifi["bssid"] = WiFi.BSSIDstr();
        wifi["channel"] = WiFi.channel();
    }
    if (src.network) {
        uint32_t disconnects = src.network->disconnectCount();
        wifi["disconnects"] = disconnects;
        if (disconnects > 0) {
            wifi["last_disconnect_reason"] = src.network->lastDisconnectReasonName();
            wifi["last_disconnect_code"] = src.network->lastDisconnectReason();
            wifi["last_disconnect_ms"] = src.network->lastDisconnectMillis();
        }
        NetLinkInfo link_info;
        src.network->linkInfo(link_info);
        WebApiJson::networkStatusJson(doc["network"].to<JsonObject>(), link_info);
    }

    // Model, "Disconnected" or "Not supported yet"; connected and stale always present
    // for the Home Assistant binary sensors
    JsonObject ups = doc["ups"].to<JsonObject>();
    WebApiJson::upsStatusJson(ups, src.ups);
    ups["connected"] = src.ups && src.ups->isConnected();
    if (!ups["stale"].is<bool>()) ups["stale"] = false;

    CrashDiag::fillJson(doc["diagnostics"].to<JsonObject>());
    if (src.update) WebApiJson::updateSummaryJson(doc["update"].to<JsonObject>(), src.update->status());

    if (src.nut) {
        const NUTServer::Stats& stats = src.nut->stats();
        JsonObject nut = doc["nut"].to<JsonObject>();
        nut["port"] = src.nut->port();
        nut["clients"] = src.nut->connectedClients();
        nut["accepted"] = stats.accepted;
        nut["rejected"] = stats.rejected;
        nut["commands"] = stats.commands;
        nut["auth_failures"] = stats.authFailures;
        nut["idle_timeouts"] = stats.idleTimeouts;
        nut["short_writes"] = stats.shortWrites;
    }

    if (src.mqtt) {
        JsonObject mqtt = doc["mqtt"].to<JsonObject>();
        mqtt["enabled"] = src.mqtt->isEnabled();
        mqtt["connected"] = src.mqtt->isConnected();
    }

    MemoryStats mem = readMemoryStats();
    JsonObject memory = doc["memory"].to<JsonObject>();
    memory["free_heap"] = mem.free_heap;
    memory["min_free_heap"] = mem.min_free_heap;
    memory["largest_free_block"] = mem.largest_free_block;
    memory["loop_stack_min_free"] = mem.loop_stack_min_free;
    if (mem.poll_stack_min_free >= 0) {
        memory["poll_stack_min_free"] = mem.poll_stack_min_free;
    }
    if (mem.hid_stack_min_free >= 0) {
        memory["hid_stack_min_free"] = mem.hid_stack_min_free;
    }
    memory["uptime_ms"] = millis();
}
