#include <unity.h>
#include <set>
#include <string>
#include "CyberPowerDriver.h"
#include "EatonDriver.h"
#include "GenericDriver.h"
#include "PowercomDriver.h"
#include "HIDParser.h"
#include "IUSBHostUPS.h"

void setUp(void) {}
void tearDown(void) {}

// Feature report 1 and input report 2 carry the same usage under different IDs,
// so the existing same-ID pruning does not apply and only the interrupt-endpoint
// check can drop the input poll.
static const uint8_t DESC[] = {
    0x05, 0x84,       // Usage Page (Power Device)
    0x09, 0x04,       // Usage (UPS)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x05, 0x85,       //   Usage Page (Battery System)
    0x09, 0x66,       //   Usage (RemainingCapacity)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0xB1, 0x02,       //   Feature
    0x85, 0x02,       //   Report ID (2)
    0x09, 0x66,       //   Usage (RemainingCapacity)
    0x81, 0x02,       //   Input
    0xC0              // End Collection
};

// One report for each case the poll planners treat differently, declared out of
// ID order: 0 precedes any Report ID, 1 is Input then Feature and 5 Feature then
// Input, 2, 9 and 255 are Input only, 3 has two fields, and 4, 6, 130, 254 and
// 255 are IDs Eaton or CyberPower exclude.
static const uint8_t MIXED_DESC[] = {
    0x05, 0x84,                          // Usage Page (Power Device)
    0x09, 0x04,                          // Usage (UPS)
    0xA1, 0x01,                          // Collection (Application)
    0x05, 0x85,                          //   Usage Page (Battery System)
    0x75, 0x08,                          //   Report Size (8)
    0x95, 0x01,                          //   Report Count (1)
    0x09, 0x66, 0x81, 0x02,              //   Input
    0x85, 0x03, 0x95, 0x02,              //   Report ID (3), Report Count (2)
    0x09, 0x66, 0x09, 0x68, 0xB1, 0x02,  //   Feature
    0x95, 0x01,                          //   Report Count (1)
    0x85, 0x01, 0x09, 0x66, 0x81, 0x02,  //   Report ID (1), Input
    0x09, 0x66, 0xB1, 0x02,              //   Feature
    0x85, 0x02, 0x09, 0x66, 0x81, 0x02,  //   Report ID (2), Input
    0x85, 0x04, 0x09, 0x66, 0xB1, 0x02,  //   Report ID (4), Feature
    0x85, 0x05, 0x09, 0x66, 0xB1, 0x02,  //   Report ID (5), Feature
    0x09, 0x66, 0x81, 0x02,              //   Input
    0x85, 0x06, 0x09, 0x66, 0xB1, 0x02,  //   Report ID (6), Feature
    0x85, 0x09, 0x09, 0x66, 0x81, 0x02,  //   Report ID (9), Input
    0x85, 0x82, 0x09, 0x66, 0xB1, 0x02,  //   Report ID (130), Feature
    0x85, 0xFE, 0x09, 0x66, 0xB1, 0x02,  //   Report ID (254), Feature
    0x85, 0xFF, 0x09, 0x66, 0x81, 0x02,  //   Report ID (255), Input
    0x85, 0x07, 0x09, 0x66, 0xB1, 0x02,  //   Report ID (7), Feature
    0xC0                                 // End Collection
};

class PollMockHost : public IUSBHostUPS {
public:
    UPSData _data;
    HIDParser _parser;
    std::vector<HIDUsageDef> _usages;
    std::set<uint8_t> _interrupt_ids;
    uint32_t _now = 0;
    std::vector<std::pair<uint8_t, uint8_t>> _requested; // {report_id, report_type}
    std::string _log; // "<ms> <I|O|F><report id>/<length>" per request

    void load(const uint8_t* desc, size_t len) {
        _parser.parseReportDescriptor(desc, len);
        _usages = _parser.getUsages();
    }

    void lock() const override {}
    void unlock() const override {}
    UPSDataLock getUPSData() const override { return UPSDataLock(_data, this); }
    String getUPSStatusString() const override { return ""; }
    bool setBeeper(bool) override { return true; }
    bool isConnected() const override { return true; }
    const HIDParser* getHIDParser() const override { return &_parser; }
    const std::vector<HIDUsageDef>& getUsages() const override { return _usages; }
    const HIDUsageDef* getUsageDef(uint32_t u) const override { return _parser.getUsageDef(u); }
    String getActiveBeeperPath() const override { return ""; }
    uint32_t getQuirks() const override { return 0; }
    bool isControlPending() const override { return false; }
    bool isInterruptReport(uint8_t report_id) const override {
        return _interrupt_ids.count(report_id) != 0;
    }
    bool requestReport(uint8_t report_id, uint8_t report_type, uint16_t length) override {
        _requested.push_back({report_id, report_type});
        char entry[32];
        snprintf(entry, sizeof(entry), "%s%u %c%u/%u", _log.empty() ? "" : ", ",
                 (unsigned)_now, "?IOF"[report_type & 3], (unsigned)report_id, (unsigned)length);
        _log += entry;
        return true;
    }
    bool requestStringDescriptor(uint8_t) override { return true; }

    bool wasRequested(uint8_t report_id, uint8_t report_type) const {
        for (const auto& r : _requested) {
            if (r.first == report_id && r.second == report_type) return true;
        }
        return false;
    }
};

// The parser files Output items as Feature, so the Output report is added by hand.
static void loadMixed(PollMockHost& host) {
    host.load(MIXED_DESC, sizeof(MIXED_DESC));
    HIDUsageDef output;
    output.usage = 0x00850066;
    output.report_id = 8;
    output.report_type = 2;
    output.found = true;
    host._usages.insert(host._usages.begin(), output);
}

static void runPollCycle(PollMockHost& host, GenericDriver& drv) {
    UPSData data;
    for (uint32_t now = 2000; now <= 4000; now += 100) {
        drv.loop(&host, data, now);
    }
}

// Calls loop() every 10 ms over [from, to).
static void runPolling(PollMockHost& host, GenericDriver& drv, uint32_t from, uint32_t to) {
    for (uint32_t now = from; now < to; now += 10) {
        host._now = now;
        drv.loop(&host, host._data, now);
    }
}

void test_polls_input_report_when_interrupt_is_silent(void) {
    PollMockHost host;
    host.load(DESC, sizeof(DESC));

    GenericDriver drv;
    drv.setup();
    runPollCycle(host, drv);

    TEST_ASSERT_TRUE(host.wasRequested(1, 3));
    TEST_ASSERT_TRUE(host.wasRequested(2, 1));
}

void test_skips_input_report_delivered_by_interrupt(void) {
    PollMockHost host;
    host.load(DESC, sizeof(DESC));
    host._interrupt_ids = {2};

    GenericDriver drv;
    drv.setup();
    runPollCycle(host, drv);

    TEST_ASSERT_TRUE_MESSAGE(host.wasRequested(1, 3), "feature reports must still be polled");
    TEST_ASSERT_FALSE_MESSAGE(host.wasRequested(2, 1), "input report already arrives on the interrupt endpoint");
}

// Each step polls the n-th planned report that is not arriving by interrupt at
// that moment, so when report 2's stream starts mid-cycle the rest of the cycle
// shifts by one and report 4 waits for the next cycle.
void test_generic_poll_sequence(void) {
    PollMockHost host;
    loadMixed(host);

    GenericDriver drv;
    drv.setup();
    runPolling(host, drv, 1000, 1310);
    host._interrupt_ids = {2};
    runPolling(host, drv, 1310, 5000);

    TEST_ASSERT_EQUAL_STRING(
        "1200 F3/3, 1250 F1/2, 1300 I2/2, 1350 F5/2, 1400 F6/2, 1450 I9/2, "
        "1500 F130/2, 1550 F254/2, 1600 I255/2, 1650 F7/2, "
        "3200 F3/3, 3250 F1/2, 3300 F4/2, 3350 F5/2, 3400 F6/2, 3450 I9/2, "
        "3500 F130/2, 3550 F254/2, 3600 I255/2, 3650 F7/2",
        host._log.c_str());
}

// Eaton keeps report 0, never polls 254 or 255, and polls input reports even
// while they arrive by interrupt.
void test_eaton_poll_sequence(void) {
    PollMockHost host;
    loadMixed(host);
    host._interrupt_ids = {2, 9};

    EatonDriver drv;
    drv.setup();
    runPolling(host, drv, 1000, 3000);

    TEST_ASSERT_EQUAL_STRING(
        "1200 I0/1, 1250 F3/3, 1300 F1/2, 1350 I2/2, 1400 F4/2, 1450 F5/2, "
        "1500 F6/2, 1550 I9/2, 1600 F130/2, 1650 F7/2",
        host._log.c_str());
}

// CyberPower polls only Feature reports outside 4, 6 and 130 up, after three
// string steps instead of four, every 30 s.
void test_cyberpower_poll_sequence(void) {
    PollMockHost host;
    loadMixed(host);

    CyberPowerDriver drv;
    drv.setup();
    runPolling(host, drv, 1000, 32000);

    TEST_ASSERT_EQUAL_STRING(
        "1150 F3/3, 1200 F1/2, 1250 F5/2, 1300 F7/2, "
        "31150 F3/3, 31200 F1/2, 31250 F5/2, 31300 F7/2",
        host._log.c_str());
}

void test_setup_replans_for_a_new_descriptor(void) {
    PollMockHost host;
    loadMixed(host);

    GenericDriver drv;
    drv.setup();
    runPolling(host, drv, 1000, 3000);

    // A reconnect replaces the descriptor, then calls setup().
    host.load(DESC, sizeof(DESC));
    drv.setup();
    host._log.clear();
    runPolling(host, drv, 3000, 5000);

    TEST_ASSERT_EQUAL_STRING("3200 F1/2, 3250 I2/2", host._log.c_str());
}

static void assertLoopPublishesDriverInfo(GenericDriver& drv, const char* name) {
    PollMockHost host;
    host.load(DESC, sizeof(DESC));
    drv.setup();
    drv.loop(&host, host._data, 1000);

    TEST_ASSERT_FALSE_MESSAGE(host._data.hasKey("ups.type"), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("esp32-nut", host._data.get("driver.name").c_str(), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("dev", host._data.get("driver.version").c_str(), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(name, host._data.get("driver.version.data").c_str(), name);
}

// Every loop() override names the firmware and sub-driver the way NUT drivers
// do, rather than putting the class name in ups.type.
void test_loops_publish_driver_info(void) {
    GenericDriver generic;
    EatonDriver eaton;
    CyberPowerDriver cyberpower;
    PowercomDriver powercom;
    assertLoopPublishesDriverInfo(generic, "GenericDriver");
    assertLoopPublishesDriverInfo(eaton, "EatonDriver");
    assertLoopPublishesDriverInfo(cyberpower, "CyberPowerDriver");
    assertLoopPublishesDriverInfo(powercom, "PowercomDriver");
}

void test_resolve_report_type_and_max_length(void) {
    HIDParser parser;
    parser.parseReportDescriptor(DESC, sizeof(DESC));

    uint8_t type = 0;
    TEST_ASSERT_TRUE(parser.resolveReportType(2, 3, type));
    TEST_ASSERT_EQUAL_UINT8(1, type); // declared as Input only, despite preferring Feature

    TEST_ASSERT_TRUE(parser.resolveReportType(1, 3, type));
    TEST_ASSERT_EQUAL_UINT8(3, type);

    TEST_ASSERT_FALSE(parser.resolveReportType(9, 3, type)); // not in the descriptor

    // Each report is one payload byte plus the ID prefix.
    TEST_ASSERT_EQUAL_UINT16(2, parser.getMaxExpectedLength());
}

#ifdef PIO_UNIT_TESTING
#ifndef ARDUINO
int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_polls_input_report_when_interrupt_is_silent);
    RUN_TEST(test_skips_input_report_delivered_by_interrupt);
    RUN_TEST(test_generic_poll_sequence);
    RUN_TEST(test_eaton_poll_sequence);
    RUN_TEST(test_cyberpower_poll_sequence);
    RUN_TEST(test_setup_replans_for_a_new_descriptor);
    RUN_TEST(test_loops_publish_driver_info);
    RUN_TEST(test_resolve_report_type_and_max_length);
    return UNITY_END();
}
#else
#include <Arduino.h>
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_polls_input_report_when_interrupt_is_silent);
    RUN_TEST(test_skips_input_report_delivered_by_interrupt);
    RUN_TEST(test_generic_poll_sequence);
    RUN_TEST(test_eaton_poll_sequence);
    RUN_TEST(test_cyberpower_poll_sequence);
    RUN_TEST(test_setup_replans_for_a_new_descriptor);
    RUN_TEST(test_loops_publish_driver_info);
    RUN_TEST(test_resolve_report_type_and_max_length);
    UNITY_END();
}
void loop() {}
#endif
#endif
