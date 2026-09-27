#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

#include <ArduinoJson.h>

// Copies each setting of each section into config, leaving the others alone
inline void mergeSettings(JsonObject config, JsonObjectConst sections) {
    for (JsonPairConst section : sections) {
        if (section.key() == "devices") {
            continue;
        }
        JsonObject target = config[section.key()];
        if (!target) {
            target = config[section.key()].to<JsonObject>();
        }
        for (JsonPairConst setting : section.value().as<JsonObjectConst>()) {
            target[setting.key()] = setting.value();
        }
    }
}

// Lays the settings built into the firmware (config.json) over the saved ones:
// the shared ones, then those under devices.<device_id>.
inline void applyBuildConfig(JsonDocument& config, JsonObjectConst build, const char* device_id) {
    JsonObject root = config.is<JsonObject>() ? config.as<JsonObject>() : config.to<JsonObject>();
    mergeSettings(root, build);
    mergeSettings(root, build["devices"][device_id]);
}

#endif // BUILD_CONFIG_H
