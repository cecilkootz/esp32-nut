#include <Arduino.h>
#include <unity.h>
#include <ArduinoJson.h>
#include <set>
#include "HaDiscovery.h"

// What fillSystemStatus() publishes on a connected board, trimmed to one of each section
static const char* STATUS_SAMPLE = R"({
  "version": "v1.6.1", "device_id": "ESP32-S3-A1B2C3", "hostname": "rack-ups",
  "mac": "02:00:00:a1:b2:c3", "uptime_s": 17405,
  "wifi": {"status": "home", "connected": true, "ssid": "home", "ip": "192.168.1.50", "rssi": -49,
           "bssid": "02:00:00:00:00:01", "channel": 1, "disconnects": 0},
  "ups": {"connected": true, "model": "Back-UPS NS 1500M2", "status": "Back-UPS NS 1500M2", "stale": false},
  "diagnostics": {"reset_reason": "SOFTWARE", "coredump_partition": true, "consecutive_restarts": 0,
                  "heap_free": 221760, "heap_min_free": 214164, "heap_largest_block": 208884, "degraded": false},
  "nut": {"port": 3493, "clients": 1, "accepted": 2, "rejected": 0, "commands": 296, "auth_failures": 0,
          "idle_timeouts": 0, "short_writes": 0},
  "mqtt": {"enabled": true, "connected": true},
  "memory": {"free_heap": 221760, "min_free_heap": 214164, "largest_free_block": 208884,
             "loop_stack_min_free": 5356, "hid_stack_min_free": 2756, "uptime_ms": 17405700}
})";

static HaDiscovery::Topics topics;
static JsonDocument payload;

static HaDiscovery::Device sampleDevice() {
    HaDiscovery::Device d;
    d.id = "ESP32-S3-A1B2C3";
    d.name = "rack-ups bridge";
    d.model = "ESP32-S3";
    d.sw_version = "v1.6.1";
    d.mac = "02:00:00:a1:b2:c3";
    d.config_url = "http://192.168.1.50/";
    return d;
}

void setUp(void) {
    topics = HaDiscovery::topicsFor("ESP32-S3-A1B2C3");
    payload.clear();
    TEST_ASSERT_FALSE(deserializeJson(payload, HaDiscovery::payload(sampleDevice(), topics).c_str()));
}

void tearDown(void) {}

// "{{ value_json.a.b }}" and "{{ 'ON' if value_json.a.b else 'OFF' }}" both give "a.b"
static std::string fieldOf(const char* tpl) {
    std::string t = tpl;
    size_t start = t.find("value_json.");
    if (start == std::string::npos) return "";
    start += strlen("value_json.");
    size_t end = t.find_first_of(" |}", start);
    return t.substr(start, end - start);
}

static bool hasField(JsonVariantConst doc, const std::string& path) {
    JsonVariantConst v = doc;
    size_t from = 0;
    while (from <= path.size()) {
        size_t dot = path.find('.', from);
        std::string key = path.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
        if (!v.is<JsonObjectConst>() || !v[key.c_str()].is<JsonVariantConst>() || v[key.c_str()].isNull()) {
            return false;
        }
        v = v[key.c_str()];
        if (dot == std::string::npos) break;
        from = dot + 1;
    }
    return true;
}

void test_topics_use_the_lowercase_device_id(void) {
    TEST_ASSERT_EQUAL_STRING("homeassistant/device/esp32-s3-a1b2c3/config", topics.discovery.c_str());
    TEST_ASSERT_EQUAL_STRING("esp32-nut/esp32-s3-a1b2c3/availability", topics.availability.c_str());
    TEST_ASSERT_EQUAL_STRING("esp32-nut/esp32-s3-a1b2c3/state", topics.state.c_str());
    TEST_ASSERT_EQUAL_STRING("esp32-nut/esp32-s3-a1b2c3/restart", topics.restart.c_str());
}

void test_device_block(void) {
    JsonObjectConst dev = payload["dev"];
    TEST_ASSERT_EQUAL_STRING("esp32-s3-a1b2c3", dev["ids"][0]);
    TEST_ASSERT_EQUAL_STRING("rack-ups bridge", dev["name"]);
    TEST_ASSERT_EQUAL_STRING("ESP32-S3", dev["mdl"]);
    TEST_ASSERT_EQUAL_STRING("v1.6.1", dev["sw"]);
    TEST_ASSERT_EQUAL_STRING("mac", dev["cns"][0][0]);
    TEST_ASSERT_EQUAL_STRING("02:00:00:a1:b2:c3", dev["cns"][0][1]);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50/", dev["cu"]);
}

void test_origin_is_present(void) {
    // HA rejects a device discovery payload without one
    TEST_ASSERT_EQUAL_STRING("esp32-nut", payload["o"]["name"]);
}

void test_shared_topics(void) {
    TEST_ASSERT_EQUAL_STRING(topics.availability.c_str(), payload["avty_t"]);
    TEST_ASSERT_EQUAL_STRING(topics.state.c_str(), payload["stat_t"]);
}

void test_every_component_has_platform_name_and_unique_id(void) {
    std::set<std::string> ids;
    for (JsonPairConst c : payload["cmps"].as<JsonObjectConst>()) {
        TEST_ASSERT_TRUE_MESSAGE(c.value()["p"].is<const char*>(), c.key().c_str());
        TEST_ASSERT_TRUE_MESSAGE(c.value()["name"].is<const char*>(), c.key().c_str());
        std::string uid = c.value()["uniq_id"].as<const char*>();
        TEST_ASSERT_EQUAL_STRING(("esp32-s3-a1b2c3_" + std::string(c.key().c_str())).c_str(), uid.c_str());
        TEST_ASSERT_TRUE_MESSAGE(ids.insert(uid).second, uid.c_str());
    }
    TEST_ASSERT_TRUE(ids.size() >= 10);
}

void test_templates_read_fields_the_status_has(void) {
    JsonDocument status;
    TEST_ASSERT_FALSE(deserializeJson(status, STATUS_SAMPLE));
    for (JsonPairConst c : payload["cmps"].as<JsonObjectConst>()) {
        const char* tpl = c.value()["val_tpl"];
        if (!tpl) continue;
        std::string field = fieldOf(tpl);
        TEST_ASSERT_FALSE_MESSAGE(field.empty(), c.key().c_str());
        TEST_ASSERT_TRUE_MESSAGE(hasField(status, field), field.c_str());
    }
}

void test_binary_sensors_map_to_on_and_off(void) {
    TEST_ASSERT_EQUAL_STRING("binary_sensor", payload["cmps"]["ups_connected"]["p"]);
    TEST_ASSERT_EQUAL_STRING("connectivity", payload["cmps"]["ups_connected"]["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("{{ 'ON' if value_json.ups.connected else 'OFF' }}",
                             payload["cmps"]["ups_connected"]["val_tpl"]);
}

void test_restart_button(void) {
    JsonObjectConst b = payload["cmps"]["restart"];
    TEST_ASSERT_EQUAL_STRING("button", b["p"]);
    TEST_ASSERT_EQUAL_STRING("restart", b["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("config", b["ent_cat"]);
    TEST_ASSERT_EQUAL_STRING(topics.restart.c_str(), b["cmd_t"]);
}

void test_noisy_diagnostics_start_disabled(void) {
    TEST_ASSERT_FALSE(payload["cmps"]["free_heap"]["en"].as<bool>());
    TEST_ASSERT_FALSE(payload["cmps"]["wifi_disconnects"]["en"].as<bool>());
    TEST_ASSERT_TRUE(payload["cmps"]["min_free_heap"]["en"].isNull());
}

void test_missing_mac_and_url_are_left_out(void) {
    HaDiscovery::Device d = sampleDevice();
    d.mac = "";
    d.config_url = "";
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, HaDiscovery::payload(d, topics).c_str()));
    TEST_ASSERT_TRUE(doc["dev"]["cns"].isNull());
    TEST_ASSERT_TRUE(doc["dev"]["cu"].isNull());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_topics_use_the_lowercase_device_id);
    RUN_TEST(test_device_block);
    RUN_TEST(test_origin_is_present);
    RUN_TEST(test_shared_topics);
    RUN_TEST(test_every_component_has_platform_name_and_unique_id);
    RUN_TEST(test_templates_read_fields_the_status_has);
    RUN_TEST(test_binary_sensors_map_to_on_and_off);
    RUN_TEST(test_restart_button);
    RUN_TEST(test_noisy_diagnostics_start_disabled);
    RUN_TEST(test_missing_mac_and_url_are_left_out);
    return UNITY_END();
}
