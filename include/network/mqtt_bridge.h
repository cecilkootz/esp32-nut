#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <Arduino.h>
#include <atomic>
#include <mutex>
#include <mqtt_client.h>
#include "HaDiscovery.h"
#include "core/config_manager.h"

// Home Assistant over MQTT: discovery, the system status and the Restart button.
// The esp-mqtt task owns the connection. Its API calls can block on the network,
// so a publisher task of ours makes them, never the loopTask, which serves NUT.
class MqttBridge {
public:
    static const uint32_t STATUS_PERIOD_MS = 30000;

    bool begin(const MqttConfig& config, const String& device_id, const String& name);
    // Right before a restart: marks the device offline in HA and disconnects
    void end();
    // Before MQTT is turned off: removes the device from HA, which would otherwise
    // keep it as unavailable, and clears its retained topics
    void forget();

    bool isEnabled() const { return _client != nullptr; }
    bool isConnected() const { return _connected; }

    // loopTask. Publishes the status every STATUS_PERIOD_MS, when the UPS link
    // goes up or down, and after (re)connecting.
    void loop(uint32_t now, bool ups_healthy, String (*buildStatus)());
    // loopTask: true once per press of the Restart button
    bool takeRestartRequest() { return _restart_requested.exchange(false); }

private:
    static const uint32_t BIT_CONNECTED = 1 << 0;
    static const uint32_t BIT_HA_ONLINE = 1 << 1;
    static const uint32_t BIT_STATUS = 1 << 2;

    static void onEvent(void* arg, esp_event_base_t base, int32_t event_id, void* event_data);
    static void publisherTask(void* arg);
    void handleEvent(esp_mqtt_event_handle_t event);
    void publishDiscovery();

    esp_mqtt_client_handle_t _client = nullptr;
    TaskHandle_t _publisher = nullptr;
    HaDiscovery::Topics _topics;
    String _device_id;
    String _name;
    // esp-mqtt keeps pointers to these for as long as the client lives
    String _host;
    String _client_id;
    String _username;
    String _password;

    std::atomic<bool> _connected{false};
    std::atomic<bool> _status_wanted{false};
    std::atomic<bool> _restart_requested{false};
    bool _forgotten = false;

    std::mutex _status_mutex;
    String _pending_status;

    uint32_t _last_status_ms = 0;
    bool _last_ups_healthy = false;

    // esp-mqtt task only: a failing broker is logged once, not on every retry
    int _last_error_type = MQTT_ERROR_TYPE_NONE;
    int _last_error_code = 0;
};

#endif // MQTT_BRIDGE_H
