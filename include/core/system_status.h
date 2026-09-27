#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

#include <ArduinoJson.h>

class AppNetworkManager;
class USBHostUPS;
class NUTServer;
class MqttBridge;

// Null pointers leave their section out
struct SystemStatusSources {
    bool ap_mode = false;
    AppNetworkManager* network = nullptr;
    USBHostUPS* ups = nullptr;
    NUTServer* nut = nullptr;
    MqttBridge* mqtt = nullptr;
};

// The document behind /api/system-status and the MQTT state topic. Call from the
// loop task: the NUT counters and the loop stack figure belong to it.
void fillSystemStatus(JsonDocument& doc, const SystemStatusSources& src);

#endif // SYSTEM_STATUS_H
