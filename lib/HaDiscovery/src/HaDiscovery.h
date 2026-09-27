#ifndef HA_DISCOVERY_H
#define HA_DISCOVERY_H

#include <Arduino.h>

// Home Assistant MQTT device discovery for the bridge itself. The UPS stays with
// HA's NUT integration; these entities read the /api/system-status document,
// which the bridge publishes to the state topic.
namespace HaDiscovery {

struct Topics {
    String discovery;     // retained device discovery payload
    String availability;  // "online" / "offline", retained, "offline" is the LWT
    String state;         // the system status JSON
    String restart;       // the Restart button's commands
};

// Keyed by the device ID, which never changes: renaming the UPS renames the
// device in HA without leaving orphaned entities behind.
Topics topicsFor(const String& device_id);

struct Device {
    String id;
    String name;
    String model;
    String sw_version;
    String mac;         // "aa:bb:cc:dd:ee:ff", links the device to router trackers
    String config_url;  // the web UI
};

String payload(const Device& device, const Topics& topics);

}  // namespace HaDiscovery

#endif  // HA_DISCOVERY_H
