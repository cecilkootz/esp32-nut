#include "core/config_manager.h"
#include <ArduinoJson.h>
#include <esp_app_desc.h>
#include "build_config_json.h"
#include "core/app_logger.h"
#include "core/build_config.h"
#include "core/device_id.h"

ConfigManager::ConfigManager() : is_valid(false) {}

static void mergeBuildConfig(JsonDocument& doc) {
    JsonDocument build;
    deserializeJson(build, BUILD_CONFIG_JSON); // checked by scripts/build_config.py
    String device_id = getDeviceId();
    applyBuildConfig(doc, build.as<JsonObjectConst>(), device_id.c_str());
    AppLogger::log("INFO", "[CONFIG] Applying the settings built into this firmware.");
    if (build["devices"].size() > 0 && build["devices"][device_id].isNull()) {
        AppLogger::log("WARN", "[CONFIG] The built-in settings have no entry for %s: shared settings only.",
                       device_id.c_str());
    }
}

bool ConfigManager::begin() {
    is_valid = false;
    
    preferences.begin("nutos", false);
    String config_json = preferences.getString("config_json", "");

    JsonDocument doc;
    if (config_json != "") {
        DeserializationError error = deserializeJson(doc, config_json);
        if (error) {
            AppLogger::log("ERROR", "[CONFIG] ERROR: JSON parsing from NVS failed. Details: %s\n", error.c_str());
            doc.clear();
        }
    }

    // Once per image, not every boot: web UI changes then last until the next update, and
    // the setup hotspot can still fix built-in Wi-Fi settings a board can't join.
    char image[65];
    esp_app_get_elf_sha256(image, sizeof(image));
    bool apply_build = BUILD_CONFIG_JSON[0] != '\0' && preferences.getString("config_image", "") != image;
    if (apply_build) {
        mergeBuildConfig(doc);
    }

    // Any section can be missing: saved before MQTT support, or left out of config.json
    JsonObjectConst wifi = doc["wifi"];
    wifi_config.ssid = wifi["ssid"] | "";
    wifi_config.password = wifi["password"] | "";

    JsonObjectConst nut = doc["nut"];
    nut_config.username = nut["username"] | "";
    nut_config.password = nut["password"] | "";
    nut_config.ups_name = nut["ups_name"] | "";

    JsonObjectConst mqtt = doc["mqtt"];
    mqtt_config.host = mqtt["host"] | "";
    mqtt_config.port = mqtt["port"] | 1883;
    mqtt_config.username = mqtt["username"] | "";
    mqtt_config.password = mqtt["password"] | "";

    if (apply_build && save()) {
        preferences.putString("config_image", image);
    }
    
    // Saving the NUT form before Wi-Fi stores an empty SSID, which station mode can't use.
    // The board stays in setup mode, and the NUT settings stay loaded so the Wi-Fi save keeps them.
    if (wifi_config.ssid.isEmpty()) {
        AppLogger::log("WARN", "[CONFIG] No Wi-Fi SSID saved: configuration incomplete.");
        return false;
    }

    is_valid = true;
    
    AppLogger::log("INFO", "[CONFIG] Configuration successfully loaded from NVS.");
    
    return true;
}

WifiConfig ConfigManager::getWifiConfig() const {
    return wifi_config;
}

NutConfig ConfigManager::getNutConfig() const {
    return nut_config;
}

MqttConfig ConfigManager::getMqttConfig() const {
    return mqtt_config;
}

bool ConfigManager::isValid() const {
    return is_valid;
}

void ConfigManager::setWifiConfig(const WifiConfig& config) {
    wifi_config = config;
}

void ConfigManager::setNutConfig(const NutConfig& config) {
    nut_config = config;
}

void ConfigManager::setMqttConfig(const MqttConfig& config) {
    mqtt_config = config;
}

bool ConfigManager::save() {
    JsonDocument doc;
    
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = wifi_config.ssid;
    wifi["password"] = wifi_config.password;
    
    JsonObject nut = doc["nut"].to<JsonObject>();
    nut["username"] = nut_config.username;
    nut["password"] = nut_config.password;
    nut["ups_name"] = nut_config.ups_name;

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["host"] = mqtt_config.host;
    mqtt["port"] = mqtt_config.port;
    mqtt["username"] = mqtt_config.username;
    mqtt["password"] = mqtt_config.password;
    
    String jsonString;
    if (serializeJson(doc, jsonString) == 0) {
        AppLogger::log("ERROR", "[CONFIG] ERROR: Cannot serialize JSON.");
        return false;
    }
    
    preferences.begin("nutos", false);
    size_t written = preferences.putString("config_json", jsonString);
    if (written == 0) {
        AppLogger::log("ERROR", "[CONFIG] ERROR: Cannot write configuration to NVS.");
        return false;
    }
    
    AppLogger::log("INFO", "[CONFIG] Configuration successfully saved to NVS.");
    return true;
}
