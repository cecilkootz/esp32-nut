#include <Arduino.h>
#include <unity.h>
#include <Preferences.h>
#include "core/config_manager.h"

ConfigManager config_manager;
Preferences test_preferences;

void setUp(void) {
    // Inizializza NVS all'inizio di ogni test
    test_preferences.begin("nutos", false);
    test_preferences.clear();
    test_preferences.end();
}

void tearDown(void) {
    // Pulizia dopo ciascun test
}

void test_save_and_load_config(void) {
    // Inizializzazione pulita
    TEST_ASSERT_FALSE(config_manager.begin()); // Dovrebbe fallire perché NVS vuota
    
    WifiConfig newWifi = {"NVS_SSID", "NVS_PASS"};
    NutConfig newNut = {"nvs_user", "nvs_nut_pass", "nvs_ups"};
    config_manager.setWifiConfig(newWifi);
    config_manager.setNutConfig(newNut);
    
    // Salva in NVS
    TEST_ASSERT_TRUE(config_manager.save());
    
    // Ricarica per verificare
    ConfigManager new_manager;
    TEST_ASSERT_TRUE(new_manager.begin());
    TEST_ASSERT_TRUE(new_manager.isValid());
    
    WifiConfig wifi = new_manager.getWifiConfig();
    TEST_ASSERT_EQUAL_STRING("NVS_SSID", wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("NVS_PASS", wifi.password.c_str());
    
    NutConfig nut = new_manager.getNutConfig();
    TEST_ASSERT_EQUAL_STRING("nvs_user", nut.username.c_str());
    TEST_ASSERT_EQUAL_STRING("nvs_nut_pass", nut.password.c_str());
    TEST_ASSERT_EQUAL_STRING("nvs_ups", nut.ups_name.c_str());
}

void test_save_and_load_mqtt_config(void) {
    config_manager.setWifiConfig({"NVS_SSID", "NVS_PASS"});
    config_manager.setNutConfig({"nvs_user", "nvs_nut_pass", "nvs_ups"});
    MqttConfig newMqtt;
    newMqtt.host = "192.168.1.10";
    newMqtt.port = 1884;
    newMqtt.username = "mqtt_user";
    newMqtt.password = "mqtt_pass";
    config_manager.setMqttConfig(newMqtt);
    TEST_ASSERT_TRUE(config_manager.save());

    ConfigManager new_manager;
    TEST_ASSERT_TRUE(new_manager.begin());
    MqttConfig mqtt = new_manager.getMqttConfig();
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", mqtt.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(1884, mqtt.port);
    TEST_ASSERT_EQUAL_STRING("mqtt_user", mqtt.username.c_str());
    TEST_ASSERT_EQUAL_STRING("mqtt_pass", mqtt.password.c_str());
}

void test_config_without_mqtt_section_loads(void) {
    // Saved by firmware from before MQTT support
    test_preferences.begin("nutos", false);
    test_preferences.putString("config_json",
        "{\"wifi\":{\"ssid\":\"s\",\"password\":\"p\"},\"nut\":{\"username\":\"u\",\"password\":\"p\",\"ups_name\":\"ups\"}}");
    test_preferences.end();

    ConfigManager new_manager;
    TEST_ASSERT_TRUE(new_manager.begin());
    MqttConfig mqtt = new_manager.getMqttConfig();
    TEST_ASSERT_TRUE(mqtt.host.isEmpty());
    TEST_ASSERT_EQUAL_UINT16(1883, mqtt.port);
}

void test_load_missing_file(void) {
    // Inizializzazione ConfigManager con NVS vuota e file inesistente
    TEST_ASSERT_FALSE(config_manager.begin());
    TEST_ASSERT_FALSE(config_manager.isValid());
}

void setup() {
    delay(2000); // Stabilizzazione porta seriale

    UNITY_BEGIN();
    RUN_TEST(test_save_and_load_config);
    RUN_TEST(test_save_and_load_mqtt_config);
    RUN_TEST(test_config_without_mqtt_section_loads);
    RUN_TEST(test_load_missing_file);
    UNITY_END();
}

void loop() {
}

