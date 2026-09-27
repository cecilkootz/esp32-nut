#include <Arduino.h>
#include <unity.h>
#include "network/hostname.h"

static const String FALLBACK = "ESP32-S3-A1B2C3";

static String hostname(const char* ups_name) {
    return hostnameFromUpsName(ups_name, FALLBACK);
}

void setUp(void) {}
void tearDown(void) {}

void test_valid_name_is_kept(void) {
    TEST_ASSERT_EQUAL_STRING("bedroom-2-tv-ups", hostname("bedroom-2-tv-ups").c_str());
}

void test_uppercase_is_lowered(void) {
    TEST_ASSERT_EQUAL_STRING("eaton3s", hostname("Eaton3S").c_str());
}

void test_invalid_characters_become_one_hyphen(void) {
    TEST_ASSERT_EQUAL_STRING("eaton-3s-garage", hostname("eaton_3s.garage").c_str());
    TEST_ASSERT_EQUAL_STRING("a-b", hostname("a__-.b").c_str());
}

void test_edge_hyphens_are_dropped(void) {
    TEST_ASSERT_EQUAL_STRING("ups", hostname("_ups_").c_str());
    TEST_ASSERT_EQUAL_STRING("ups", hostname("-ups-").c_str());
}

void test_non_ascii_is_dropped(void) {
    TEST_ASSERT_EQUAL_STRING("ps-1", hostname("\xC3\x9CPS-1").c_str());
}

void test_long_name_is_cut_to_31(void) {
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz01234",
                             hostname("abcdefghijklmnopqrstuvwxyz0123456789").c_str());
}

void test_cut_does_not_end_on_a_hyphen(void) {
    // The 31st character is the hyphen
    TEST_ASSERT_EQUAL_STRING("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                             hostname("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_b").c_str());
}

void test_unusable_name_falls_back(void) {
    TEST_ASSERT_EQUAL_STRING(FALLBACK.c_str(), hostname("").c_str());
    TEST_ASSERT_EQUAL_STRING(FALLBACK.c_str(), hostname("_._").c_str());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_name_is_kept);
    RUN_TEST(test_uppercase_is_lowered);
    RUN_TEST(test_invalid_characters_become_one_hyphen);
    RUN_TEST(test_edge_hyphens_are_dropped);
    RUN_TEST(test_non_ascii_is_dropped);
    RUN_TEST(test_long_name_is_cut_to_31);
    RUN_TEST(test_cut_does_not_end_on_a_hyphen);
    RUN_TEST(test_unusable_name_falls_back);
    return UNITY_END();
}
