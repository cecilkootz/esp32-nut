#include <Arduino.h>
#include <unity.h>
#include "core/restart_guard.h"

void setUp(void) {}
void tearDown(void) {}

static const char* blocker(const char* status, bool fresh = true, bool fsd = false) {
    return restartBlocker(status, fresh, fsd);
}

void test_online_allows_restart(void) {
    TEST_ASSERT_NULL(blocker("OL"));
    TEST_ASSERT_NULL(blocker("OL CHRG"));
    TEST_ASSERT_NULL(blocker("OL BOOST"));
    TEST_ASSERT_NULL(blocker("UNKNOWN"));
}

void test_on_battery_blocks(void) {
    TEST_ASSERT_EQUAL_STRING("OB", blocker("OB DISCHRG"));
    TEST_ASSERT_EQUAL_STRING("OB", blocker("OB DISCHRG LB"));
}

void test_low_battery_while_online_allows_restart(void) {
    // upsmon only treats OB and LB together as critical
    TEST_ASSERT_NULL(blocker("OL CHRG LB"));
}

void test_states_upsmon_calls_dead_without_contact_block(void) {
    TEST_ASSERT_EQUAL_STRING("CAL", blocker("OL CAL"));
    TEST_ASSERT_EQUAL_STRING("BYPASS", blocker("OL BYPASS"));
    TEST_ASSERT_EQUAL_STRING("OFF", blocker("OFF"));
    TEST_ASSERT_EQUAL_STRING("OVER", blocker("OL OVER"));
    TEST_ASSERT_EQUAL_STRING("ALARM", blocker("OL ALARM"));
    TEST_ASSERT_EQUAL_STRING("FSD", blocker("OB LB FSD"));
}

void test_whole_tokens_only(void) {
    TEST_ASSERT_NULL(blocker("OL OFFLINEX"));
    TEST_ASSERT_NULL(blocker("OL XOB"));
}

void test_stale_data_allows_restart(void) {
    TEST_ASSERT_NULL(blocker("OB DISCHRG", false));
}

void test_nut_forced_shutdown_blocks_even_without_data(void) {
    TEST_ASSERT_EQUAL_STRING("FSD", blocker("OL", true, true));
    TEST_ASSERT_EQUAL_STRING("FSD", blocker("UNKNOWN", false, true));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_online_allows_restart);
    RUN_TEST(test_on_battery_blocks);
    RUN_TEST(test_low_battery_while_online_allows_restart);
    RUN_TEST(test_states_upsmon_calls_dead_without_contact_block);
    RUN_TEST(test_whole_tokens_only);
    RUN_TEST(test_stale_data_allows_restart);
    RUN_TEST(test_nut_forced_shutdown_blocks_even_without_data);
    return UNITY_END();
}
