#include "network/mqtt_bridge.h"
#include <WiFi.h>
#include "core/app_logger.h"
#include "core/device_id.h"
#include "core/version.h"

static const char* HA_STATUS_TOPIC = "homeassistant/status";
static const uint32_t PUBLISHER_STACK = 6144;

static bool matches(const char* data, int len, const char* expected) {
    return len == (int)strlen(expected) && memcmp(data, expected, len) == 0;
}

bool MqttBridge::begin(const MqttConfig& config, const String& device_id, const String& name) {
    if (config.host.length() == 0) {
        return false;
    }
    _device_id = device_id;
    _name = name;
    _topics = HaDiscovery::topicsFor(device_id);
    _host = config.host;
    _client_id = "esp32-nut-" + device_id;
    _client_id.toLowerCase();
    _username = config.username;
    _password = config.password;

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname = _host.c_str();
    cfg.broker.address.port = config.port;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    cfg.credentials.client_id = _client_id.c_str();
    if (_username.length() > 0) {
        cfg.credentials.username = _username.c_str();
    }
    if (_password.length() > 0) {
        cfg.credentials.authentication.password = _password.c_str();
    }
    cfg.session.last_will.topic = _topics.availability.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    // The broker publishes the will 1.5 keepalives after the board goes silent
    cfg.session.keepalive = 15;
    // Bounds how long end() can hold up a restart
    cfg.network.timeout_ms = 5000;
    // Discovery is about 3 KB and the status about 1.5 KB: send each in one piece
    cfg.buffer.out_size = 4096;

    _client = esp_mqtt_client_init(&cfg);
    if (!_client) {
        AppLogger::log("ERROR", "[MQTT] Client init failed");
        return false;
    }
    esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, &MqttBridge::onEvent, this);
    if (xTaskCreate(publisherTask, "mqtt_pub", PUBLISHER_STACK, this, 1, &_publisher) != pdPASS) {
        AppLogger::log("ERROR", "[MQTT] Publisher task creation failed");
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }
    esp_err_t err = esp_mqtt_client_start(_client);
    if (err != ESP_OK) {
        AppLogger::log("ERROR", "[MQTT] Client start failed: %s", esp_err_to_name(err));
        vTaskDelete(_publisher);
        _publisher = nullptr;
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }
    AppLogger::log("INFO", "[MQTT] Connecting to %s:%u as %s", _host.c_str(), (unsigned)config.port,
                   _client_id.c_str());
    return true;
}

void MqttBridge::end() {
    if (!_client) {
        return;
    }
    if (_connected && !_forgotten) {
        esp_mqtt_client_publish(_client, _topics.availability.c_str(), "offline", 0, 1, 1);
    }
    // Sends DISCONNECT, after which the broker drops the will
    esp_mqtt_client_stop(_client);
}

void MqttBridge::forget() {
    if (!_client || !_connected) {
        return;
    }
    // An empty retained message deletes the retained one
    esp_mqtt_client_publish(_client, _topics.discovery.c_str(), "", 0, 1, 1);
    esp_mqtt_client_publish(_client, _topics.availability.c_str(), "", 0, 1, 1);
    _forgotten = true;
}

void MqttBridge::loop(uint32_t now, bool ups_healthy, String (*buildStatus)()) {
    if (!_client || !_connected) {
        return;
    }
    bool due = _status_wanted.exchange(false) || ups_healthy != _last_ups_healthy ||
               now - _last_status_ms >= STATUS_PERIOD_MS;
    _last_ups_healthy = ups_healthy;
    if (!due) {
        return;
    }
    _last_status_ms = now;
    String status = buildStatus();
    {
        std::lock_guard<std::mutex> lock(_status_mutex);
        _pending_status = status;
    }
    xTaskNotify(_publisher, BIT_STATUS, eSetBits);
}

void MqttBridge::onEvent(void* arg, esp_event_base_t, int32_t, void* event_data) {
    static_cast<MqttBridge*>(arg)->handleEvent(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttBridge::handleEvent(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        _connected = true;
        _last_error_type = MQTT_ERROR_TYPE_NONE;
        _last_error_code = 0;
        AppLogger::log("INFO", "[MQTT] Connected to the broker");
        xTaskNotify(_publisher, BIT_CONNECTED, eSetBits);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (_connected) {
            AppLogger::log("WARN", "[MQTT] Disconnected from the broker");
        }
        _connected = false;
        break;
    case MQTT_EVENT_DATA:
        // A retained press would restart the board every time it connects
        if (event->retain || event->current_data_offset != 0) {
            break;
        }
        if (matches(event->topic, event->topic_len, _topics.restart.c_str())) {
            if (matches(event->data, event->data_len, "PRESS")) {
                _restart_requested = true;
            }
        } else if (matches(event->topic, event->topic_len, HA_STATUS_TOPIC)) {
            // A restarted HA would otherwise show no state until the next periodic status
            if (matches(event->data, event->data_len, "online")) {
                xTaskNotify(_publisher, BIT_HA_ONLINE, eSetBits);
            }
        }
        break;
    case MQTT_EVENT_ERROR: {
        const esp_mqtt_error_codes_t* err = event->error_handle;
        int code = err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED ? (int)err->connect_return_code
                                                                         : err->esp_transport_sock_errno;
        // The client retries every 10 s: log a failure once, not on every retry
        if (err->error_type == _last_error_type && code == _last_error_code) {
            break;
        }
        _last_error_type = err->error_type;
        _last_error_code = code;
        if (err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            AppLogger::log("ERROR", "[MQTT] Broker refused the connection (code %d%s)", code,
                           code == MQTT_CONNECTION_REFUSE_BAD_USERNAME || code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                               ? ": check the username and password" : "");
        } else if (err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            AppLogger::log("ERROR", "[MQTT] Cannot reach the broker (%s, errno %d)",
                           esp_err_to_name(err->esp_tls_last_esp_err), code);
        }
        break;
    }
    default:
        break;
    }
}

void MqttBridge::publisherTask(void* arg) {
    MqttBridge* self = static_cast<MqttBridge*>(arg);
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if (!self->_connected) {
            continue;
        }
        if (bits & BIT_CONNECTED) {
            esp_mqtt_client_publish(self->_client, self->_topics.availability.c_str(), "online", 0, 1, 1);
            esp_mqtt_client_subscribe_single(self->_client, self->_topics.restart.c_str(), 1);
            esp_mqtt_client_subscribe_single(self->_client, HA_STATUS_TOPIC, 1);
        }
        if (bits & (BIT_CONNECTED | BIT_HA_ONLINE)) {
            self->publishDiscovery();
            self->_status_wanted = true;
        }
        if (bits & BIT_STATUS) {
            String status;
            {
                std::lock_guard<std::mutex> lock(self->_status_mutex);
                status = self->_pending_status;
                self->_pending_status = "";
            }
            if (status.length() > 0) {
                esp_mqtt_client_publish(self->_client, self->_topics.state.c_str(), status.c_str(),
                                        status.length(), 0, 0);
            }
        }
    }
}

void MqttBridge::publishDiscovery() {
    HaDiscovery::Device device;
    device.id = _device_id;
    device.name = _name;
    device.model = ESP.getChipModel();
    device.sw_version = FIRMWARE_VERSION;
    device.mac = getMacAddress();
    device.config_url = "http://" + WiFi.localIP().toString() + "/";
    String payload = HaDiscovery::payload(device, _topics);
    esp_mqtt_client_publish(_client, _topics.discovery.c_str(), payload.c_str(), payload.length(), 1, 1);
}
