#include <Arduino.h>
#include <unity.h>
#include <ArduinoJson.h>
#include "core/build_config.h"

static const char* DEVICE = "ESP32-S3-A1B2C3";

static JsonDocument saved;

static void apply(const char* build_json) {
    JsonDocument build;
    TEST_ASSERT_FALSE(deserializeJson(build, build_json));
    applyBuildConfig(saved, build.as<JsonObjectConst>(), DEVICE);
}

static const char* setting(const char* section, const char* key) {
    return saved[section][key];
}

void setUp(void) {
    saved.clear();
    deserializeJson(saved, R"({"wifi": {"ssid": "old", "password": "old-pass"},
        "nut": {"username": "monuser", "password": "nut-pass", "ups_name": "saved-ups"},
        "mqtt": {"host": "", "port": 1883, "username": "", "password": ""}})");
}

void tearDown(void) {}

void test_built_in_settings_replace_saved_ones(void) {
    apply(R"({"wifi": {"ssid": "home"}, "mqtt": {"host": "broker", "port": 1884}})");
    TEST_ASSERT_EQUAL_STRING("home", setting("wifi", "ssid"));
    TEST_ASSERT_EQUAL_STRING("broker", setting("mqtt", "host"));
    TEST_ASSERT_EQUAL_INT(1884, saved["mqtt"]["port"].as<int>());
}

void test_settings_left_out_keep_their_value(void) {
    apply(R"({"wifi": {"ssid": "home"}})");
    TEST_ASSERT_EQUAL_STRING("old-pass", setting("wifi", "password"));
    TEST_ASSERT_EQUAL_STRING("saved-ups", setting("nut", "ups_name"));
    TEST_ASSERT_EQUAL_STRING("nut-pass", setting("nut", "password"));
}

void test_settings_missing_from_a_saved_section_are_added(void) {
    saved["nut"].remove("ups_name");
    apply(R"({"nut": {"ups_name": "rack-ups"}})");
    TEST_ASSERT_EQUAL_STRING("rack-ups", setting("nut", "ups_name"));
}

void test_device_settings_win_over_shared_ones(void) {
    apply(R"({"nut": {"username": "upsmon", "ups_name": "shared"},
        "devices": {"ESP32-S3-A1B2C3": {"nut": {"ups_name": "rack-ups"}}}})");
    TEST_ASSERT_EQUAL_STRING("rack-ups", setting("nut", "ups_name"));
    TEST_ASSERT_EQUAL_STRING("upsmon", setting("nut", "username"));
}

void test_other_devices_are_ignored(void) {
    apply(R"({"devices": {"ESP32-S3-D4E5F6": {"nut": {"ups_name": "desk-ups"}}}})");
    TEST_ASSERT_EQUAL_STRING("saved-ups", setting("nut", "ups_name"));
    TEST_ASSERT_TRUE(saved["devices"].isNull());
}

void test_unconfigured_board_gets_every_built_in_setting(void) {
    saved.clear();
    apply(R"({"wifi": {"ssid": "home", "password": "secret"},
        "devices": {"ESP32-S3-A1B2C3": {"nut": {"ups_name": "rack-ups"}}}})");
    TEST_ASSERT_EQUAL_STRING("home", setting("wifi", "ssid"));
    TEST_ASSERT_EQUAL_STRING("secret", setting("wifi", "password"));
    TEST_ASSERT_EQUAL_STRING("rack-ups", setting("nut", "ups_name"));
}

void test_settings_outlive_the_built_in_document(void) {
    // ConfigManager parses the built-in settings into a local document
    apply(R"({"wifi": {"password": "p@ss \"quoted\" $HOME"}})");
    JsonDocument scribble;
    deserializeJson(scribble, R"({"wifi": {"password": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}})");
    TEST_ASSERT_EQUAL_STRING("p@ss \"quoted\" $HOME", setting("wifi", "password"));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_built_in_settings_replace_saved_ones);
    RUN_TEST(test_settings_left_out_keep_their_value);
    RUN_TEST(test_settings_missing_from_a_saved_section_are_added);
    RUN_TEST(test_device_settings_win_over_shared_ones);
    RUN_TEST(test_other_devices_are_ignored);
    RUN_TEST(test_unconfigured_board_gets_every_built_in_setting);
    RUN_TEST(test_settings_outlive_the_built_in_document);
    return UNITY_END();
}
