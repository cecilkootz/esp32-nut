#include "HaDiscovery.h"
#include <ArduinoJson.h>
#include <memory>

namespace HaDiscovery {

namespace {

String lowercase(const String& s) {
    String out = s;
    out.toLowerCase();
    return out;
}

JsonObject addComponent(JsonObject components, const String& device_id, const char* id,
                        const char* platform, const char* name) {
    JsonObject c = components[id].to<JsonObject>();
    c["p"] = platform;
    c["name"] = name;
    c["uniq_id"] = lowercase(device_id) + "_" + id;
    c["ent_cat"] = "diagnostic";
    return c;
}

JsonObject addBinarySensor(JsonObject components, const String& device_id, const char* id,
                           const char* name, const char* device_class, const char* field) {
    JsonObject c = addComponent(components, device_id, id, "binary_sensor", name);
    c["dev_cla"] = device_class;
    c["val_tpl"] = String("{{ 'ON' if value_json.") + field + " else 'OFF' }}";
    return c;
}

JsonObject addSensor(JsonObject components, const String& device_id, const char* id,
                     const char* name, const char* field) {
    JsonObject c = addComponent(components, device_id, id, "sensor", name);
    c["val_tpl"] = String("{{ value_json.") + field + " }}";
    return c;
}

JsonObject addHeapSensor(JsonObject components, const String& device_id, const char* id,
                         const char* name, const char* field) {
    JsonObject c = addSensor(components, device_id, id, name, field);
    c["dev_cla"] = "data_size";
    c["unit_of_meas"] = "B";
    c["stat_cla"] = "measurement";
    return c;
}

}  // namespace

Topics topicsFor(const String& device_id) {
    String id = lowercase(device_id);
    String base = "esp32-nut/" + id;
    Topics t;
    t.discovery = "homeassistant/device/" + id + "/config";
    t.availability = base + "/availability";
    t.state = base + "/state";
    t.restart = base + "/restart";
    return t;
}

String payload(const Device& device, const Topics& topics) {
    JsonDocument doc;

    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"].to<JsonArray>().add(lowercase(device.id));
    dev["name"] = device.name;
    dev["mf"] = "Espressif";
    dev["mdl"] = device.model;
    dev["sw"] = device.sw_version;
    if (device.mac.length() > 0) {
        JsonArray mac = dev["cns"].to<JsonArray>().add<JsonArray>();
        mac.add("mac");
        mac.add(device.mac);
    }
    if (device.config_url.length() > 0) {
        dev["cu"] = device.config_url;
    }

    JsonObject origin = doc["o"].to<JsonObject>();
    origin["name"] = "esp32-nut";
    origin["sw"] = device.sw_version;

    // Inherited by every component that uses them
    doc["avty_t"] = topics.availability;
    doc["stat_t"] = topics.state;

    JsonObject cmps = doc["cmps"].to<JsonObject>();
    const String& id = device.id;

    addBinarySensor(cmps, id, "ups_connected", "UPS link", "connectivity", "ups.connected");
    addBinarySensor(cmps, id, "ups_stale", "UPS data stale", "problem", "ups.stale");
    addBinarySensor(cmps, id, "degraded", "Degraded mode", "problem", "diagnostics.degraded");

    JsonObject rssi = addSensor(cmps, id, "wifi_signal", "Wi-Fi signal", "wifi.rssi");
    rssi["dev_cla"] = "signal_strength";
    rssi["unit_of_meas"] = "dBm";
    rssi["stat_cla"] = "measurement";

    JsonObject disconnects = addSensor(cmps, id, "wifi_disconnects", "Wi-Fi disconnects", "wifi.disconnects");
    disconnects["stat_cla"] = "total_increasing";
    disconnects["en"] = false;

    addSensor(cmps, id, "ip", "IP address", "wifi.ip");

    JsonObject uptime = addSensor(cmps, id, "uptime", "Uptime", "uptime_s");
    uptime["dev_cla"] = "duration";
    uptime["unit_of_meas"] = "s";
    uptime["stat_cla"] = "total_increasing";

    // The attributes carry the last crash and the cause of the last controlled restart
    JsonObject reset = addSensor(cmps, id, "reset_reason", "Last reset reason", "diagnostics.reset_reason");
    reset["json_attr_t"] = topics.state;
    reset["json_attr_tpl"] = "{{ value_json.diagnostics | tojson }}";

    addHeapSensor(cmps, id, "min_free_heap", "Minimum free heap", "memory.min_free_heap");
    addHeapSensor(cmps, id, "free_heap", "Free heap", "memory.free_heap")["en"] = false;
    addHeapSensor(cmps, id, "largest_free_block", "Largest free heap block", "memory.largest_free_block")["en"] = false;

    addSensor(cmps, id, "nut_clients", "NUT clients", "nut.clients")["stat_cla"] = "measurement";

    JsonObject restart = addComponent(cmps, id, "restart", "button", "Restart");
    restart["dev_cla"] = "restart";
    restart["ent_cat"] = "config";
    restart["cmd_t"] = topics.restart;

    // Through a buffer: the native tests' String has no write() for ArduinoJson
    size_t len = measureJson(doc);
    std::unique_ptr<char[]> buf(new char[len + 1]);
    serializeJson(doc, buf.get(), len + 1);
    return String(buf.get());
}

}  // namespace HaDiscovery
