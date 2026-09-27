#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

struct WifiConfig {
    String ssid;
    String password;
};

struct NutConfig {
    String username;
    String password;
    String ups_name;
};

struct MqttConfig {
    String host; // empty: MQTT off
    uint16_t port = 1883;
    String username;
    String password;
};

class ConfigManager {
public:
    ConfigManager();
    bool begin();
    
    WifiConfig getWifiConfig() const;
    NutConfig getNutConfig() const;
    MqttConfig getMqttConfig() const;
    bool isValid() const;
    
    void setWifiConfig(const WifiConfig& config);
    void setNutConfig(const NutConfig& config);
    void setMqttConfig(const MqttConfig& config);
    bool save();

private:
    WifiConfig wifi_config;
    NutConfig nut_config;
    MqttConfig mqtt_config;
    bool is_valid;
    Preferences preferences;
};

#endif // CONFIG_MANAGER_H
