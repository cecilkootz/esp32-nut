#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

#include <ArduinoJson.h>

class AppNetworkManager;
class IUpsSource;
class NUTServer;
class MqttBridge;
class UpdateService;

// Null pointers leave their section out
struct SystemStatusSources {
    AppNetworkManager* network = nullptr;
    IUpsSource* ups = nullptr;
    NUTServer* nut = nullptr;
    MqttBridge* mqtt = nullptr;
    UpdateService* update = nullptr;
};

// The document behind /api/system-status and the MQTT state topic. Call from the
// loop task: the NUT counters and the loop stack figure belong to it.
void fillSystemStatus(JsonDocument& doc, const SystemStatusSources& src);

#endif // SYSTEM_STATUS_H
