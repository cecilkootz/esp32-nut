#include <unity.h>
#include "UPSData.h"

void setUp(void) {}
void tearDown(void) {}

void test_status_online_normal(void) {
    UPSData data;
    data.set("ups.status.ac_present", "1");
    data.set("ups.status.discharging", "0");

    TEST_ASSERT_EQUAL_STRING("OL", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_on_battery_discharging(void) {
    UPSData data;
    data.set("ups.status.ac_present", "0");
    data.set("ups.status.discharging", "1");

    TEST_ASSERT_EQUAL_STRING("OB", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_on_battery_without_discharging_flag(void) {
    UPSData data;
    data.set("ups.status.ac_present", "0");
    data.set("ups.status.discharging", "0");

    TEST_ASSERT_EQUAL_STRING("OB", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_on_battery_low_battery(void) {
    UPSData data;
    data.set("ups.status.ac_present", "0");
    data.set("ups.status.discharging", "1");
    data.set("ups.status.battery_low", "1");

    TEST_ASSERT_EQUAL_STRING("OB LB", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_online_charging(void) {
    UPSData data;
    data.set("ups.status.ac_present", "1");
    data.set("ups.status.charging", "1");
    data.set("battery.charge", "80");

    TEST_ASSERT_EQUAL_STRING("OL CHRG", UPSData::computeUPSStatusString(data).c_str());

    // Charging flag ignored when at 100% on AC
    data.set("battery.charge", "100");
    TEST_ASSERT_EQUAL_STRING("OL", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_multiple_alarm_flags(void) {
    UPSData data;
    data.set("ups.status.ac_present", "1");
    data.set("ups.status.overload", "1");
    data.set("ups.status.replace_battery", "1");

    TEST_ASSERT_EQUAL_STRING("OL RB OVER", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_shutdown_imminent_without_comm_lost_token(void) {
    UPSData data;
    data.set("ups.status.shutdown_imminent", "1");
    data.set("ups.status.comm_lost", "1");

    // COMM_LOST is not a NUT status token, so no client recognises it.
    TEST_ASSERT_EQUAL_STRING("FSD", UPSData::computeUPSStatusString(data).c_str());
}

void test_status_empty_data_returns_unknown(void) {
    UPSData data;
    TEST_ASSERT_EQUAL_STRING("Unknown", UPSData::computeUPSStatusString(data).c_str());
}

#ifdef PIO_UNIT_TESTING
#ifndef ARDUINO
int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_status_online_normal);
    RUN_TEST(test_status_on_battery_discharging);
    RUN_TEST(test_status_on_battery_without_discharging_flag);
    RUN_TEST(test_status_on_battery_low_battery);
    RUN_TEST(test_status_online_charging);
    RUN_TEST(test_status_multiple_alarm_flags);
    RUN_TEST(test_status_shutdown_imminent_without_comm_lost_token);
    RUN_TEST(test_status_empty_data_returns_unknown);
    return UNITY_END();
}
#else
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_status_online_normal);
    RUN_TEST(test_status_on_battery_discharging);
    RUN_TEST(test_status_on_battery_without_discharging_flag);
    RUN_TEST(test_status_on_battery_low_battery);
    RUN_TEST(test_status_online_charging);
    RUN_TEST(test_status_multiple_alarm_flags);
    RUN_TEST(test_status_shutdown_imminent_without_comm_lost_token);
    RUN_TEST(test_status_empty_data_returns_unknown);
    UNITY_END();
}
void loop() {}
#endif
#endif
