#include "core/system_status.h"
#include <WiFi.h>
#include <esp_timer.h>
#include "NUTServer.h"
#include "USBHostUPS.h"
#include "core/crash_diag.h"
#include "core/device_id.h"
#include "core/memory_stats.h"
#include "core/version.h"
#include "network/mqtt_bridge.h"
#include "network/network_manager.h"

void fillSystemStatus(JsonDocument& doc, const SystemStatusSources& src) {
    doc["version"] = FIRMWARE_VERSION;
    doc["device_id"] = getDeviceId();
    doc["hostname"] = WiFi.getHostname();
    doc["mac"] = getMacAddress();
    // millis() wraps after 49.7 days
    doc["uptime_s"] = (uint32_t)(esp_timer_get_time() / 1000000);

    // Wi-Fi status
    wl_status_t wifi_status = WiFi.status();
    bool wifi_connected = !src.ap_mode && wifi_status == WL_CONNECTED;
    String wifi_status_str = "Disconnected";
    if (src.ap_mode) {
        wifi_status_str = "AP Mode Active";
    } else if (wifi_connected) {
        wifi_status_str = WiFi.SSID();
    } else {
        wifi_status_str = "Connecting";
    }
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    // The web UI shows status; connected and ssid are for other clients
    wifi["status"] = wifi_status_str;
    wifi["connected"] = wifi_connected;
    if (wifi_connected) {
        wifi["ssid"] = WiFi.SSID();
        wifi["ip"] = WiFi.localIP().toString();
        wifi["rssi"] = WiFi.RSSI();
        wifi["bssid"] = WiFi.BSSIDstr();
        wifi["channel"] = WiFi.channel();
    }
    if (src.network && !src.ap_mode) {
        uint32_t disconnects = src.network->disconnectCount();
        wifi["disconnects"] = disconnects;
        if (disconnects > 0) {
            wifi["last_disconnect_reason"] = src.network->lastDisconnectReasonName();
            wifi["last_disconnect_code"] = src.network->lastDisconnectReason();
            wifi["last_disconnect_ms"] = src.network->lastDisconnectMillis();
        }
    }

    // UPS status
    JsonObject ups = doc["ups"].to<JsonObject>();
    bool ups_connected = src.ups && src.ups->isConnected();
    ups["connected"] = ups_connected;
    if (ups_connected) {
        auto data = src.ups->getUPSData();
        if (data->hasKey("ups.model")) {
            String model = data->get("ups.model");
            ups["model"] = model;
            ups["status"] = model;
        } else {
            ups["status"] = "Unknown Model";
        }
    } else {
        ups["status"] = "Disconnected";
    }
    // A missing UPS is already "Disconnected": stale is for an attached one
    ups["stale"] = ups_connected && src.ups->isDataStale();

    CrashDiag::fillJson(doc["diagnostics"].to<JsonObject>());

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
