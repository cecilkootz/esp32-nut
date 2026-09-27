#include <unity.h>
#include "GenericDriver.h"
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

class PollMockHost : public IUSBHostUPS {
public:
    UPSData _data;
    HIDParser _parser;
    bool _interrupt_active = false;
    std::vector<std::pair<uint8_t, uint8_t>> _requested; // {report_id, report_type}

    void lock() const override {}
    void unlock() const override {}
    UPSDataLock getUPSData() const override { return UPSDataLock(_data, this); }
    String getUPSStatusString() const override { return ""; }
    bool setBeeper(bool) override { return true; }
    bool isConnected() const override { return true; }
    const HIDParser* getHIDParser() const override { return &_parser; }
    const std::vector<HIDUsageDef>& getUsages() const override { return _parser.getUsages(); }
    const HIDUsageDef* getUsageDef(uint32_t u) const override { return _parser.getUsageDef(u); }
    String getActiveBeeperPath() const override { return ""; }
    uint32_t getQuirks() const override { return 0; }
    bool isControlPending() const override { return false; }
    bool isInterruptReport(uint8_t report_id) const override {
        return _interrupt_active && report_id == 2;
    }
    bool requestReport(uint8_t report_id, uint8_t report_type, uint16_t) override {
        _requested.push_back({report_id, report_type});
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

static void runPollCycle(PollMockHost& host, GenericDriver& drv) {
    UPSData data;
    for (uint32_t now = 2000; now <= 4000; now += 100) {
        drv.loop(&host, data, now);
    }
}

void test_polls_input_report_when_interrupt_is_silent(void) {
    PollMockHost host;
    host._parser.parseReportDescriptor(DESC, sizeof(DESC));
    host._interrupt_active = false;

    GenericDriver drv;
    drv.setup();
    runPollCycle(host, drv);

    TEST_ASSERT_TRUE(host.wasRequested(1, 3));
    TEST_ASSERT_TRUE(host.wasRequested(2, 1));
}

void test_skips_input_report_delivered_by_interrupt(void) {
    PollMockHost host;
    host._parser.parseReportDescriptor(DESC, sizeof(DESC));
    host._interrupt_active = true;

    GenericDriver drv;
    drv.setup();
    runPollCycle(host, drv);

    TEST_ASSERT_TRUE_MESSAGE(host.wasRequested(1, 3), "feature reports must still be polled");
    TEST_ASSERT_FALSE_MESSAGE(host.wasRequested(2, 1), "input report already arrives on the interrupt endpoint");
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
    RUN_TEST(test_resolve_report_type_and_max_length);
    return UNITY_END();
}
#else
#include <Arduino.h>
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_polls_input_report_when_interrupt_is_silent);
    RUN_TEST(test_skips_input_report_delivered_by_interrupt);
    RUN_TEST(test_resolve_report_type_and_max_length);
    UNITY_END();
}
void loop() {}
#endif
#endif
