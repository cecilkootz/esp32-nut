#include "GoldenMateDriver.h"
#include "HIDParser.h"
#include "IUSBHostUPS.h"
#include <string.h>

void GoldenMateDriver::decodeReport(IUSBHostUPS* host, uint8_t report_id, uint8_t report_type,
                                    const uint8_t* data, size_t length, UPSData& ups_data) {
    if (!host || !data || length == 0) return;

    if (report_id == 1 && report_type == 3 &&
        length < host->getHIDParser()->getExpectedLength(report_id, report_type)) return;

    // This BMS occasionally returns a Feature report with its status bits and
    // other live fields zeroed. Keep the last valid status for two such reads;
    // repeated ambiguous reads clear it instead of claiming the UPS is online.
    bool has_ac = false, has_discharging = false, has_charging = false;
    bool has_low = false, low = false;
    bool has_battery = false;
    bool ac = false, discharging = false, charging = false, battery = false;
    for (const auto& usage : host->getUsages()) {
        if (usage.report_id != report_id || usage.report_type != report_type) continue;
        const char* path = usage.path;
        if (strcmp(path, "UPS.PowerSummary.PresentStatus.ACPresent") == 0) {
            has_ac = true;
            ac = HIDParser::extractUsage(&usage, report_id, data, length) != 0;
        } else if (strcmp(path, "UPS.PowerSummary.PresentStatus.Discharging") == 0) {
            has_discharging = true;
            discharging = HIDParser::extractUsage(&usage, report_id, data, length) != 0;
        } else if (strcmp(path, "UPS.PowerSummary.PresentStatus.Charging") == 0) {
            has_charging = true;
            charging = HIDParser::extractUsage(&usage, report_id, data, length) != 0;
        } else if (strcmp(path, "UPS.PowerSummary.PresentStatus.BelowRemainingCapacityLimit") == 0) {
            has_low = true;
            low = HIDParser::extractUsage(&usage, report_id, data, length) != 0;
        } else if (strcmp(path, "UPS.PowerSummary.PresentStatus.BatteryPresent") == 0) {
            has_battery = true;
            battery = HIDParser::extractUsage(&usage, report_id, data, length) != 0;
        }
    }
    if (has_ac && has_discharging && has_charging && has_battery) {
        if (battery && !ac && !discharging && !charging && (!has_low || !low)) {
            if (_ambiguousStatusReports < 3) ++_ambiguousStatusReports;
            if (_ambiguousStatusReports >= 3) {
                ups_data.remove("ups.status.ac_present");
                ups_data.remove("ups.status.discharging");
                ups_data.remove("ups.status.charging");
                ups_data.remove("ups.status.battery_low");
            }
            return;
        }
        _ambiguousStatusReports = 0;
    }

    GenericDriver::decodeReport(host, report_id, report_type, data, length, ups_data);

    // These paths follow NUT's idowell-hid mapping for GoldenMate. The generic
    // driver treats PowerSummary.ConfigVoltage as the nominal input voltage.
    for (const auto& usage : host->getUsages()) {
        if (usage.report_id != report_id || usage.report_type != report_type) continue;
        const char* path = usage.path;
        if (strcmp(path, "UPS.PowerSummary.ConfigVoltage") == 0) {
            ups_data.remove("input.voltage.nominal");
            double value = HIDParser::extractUsage(&usage, report_id, data, length);
            if (value > 0) ups_data.set("battery.voltage.nominal", String(value, 1));
        } else if (strcmp(path, "UPS.PowerSummary.DesignCapacity") == 0) {
            double value = HIDParser::extractUsage(&usage, report_id, data, length);
            if (value > 0) ups_data.set("battery.capacity.design", String((int)value));
        } else if (strcmp(path, "UPS.PowerSummary.FullChargeCapacity") == 0) {
            double value = HIDParser::extractUsage(&usage, report_id, data, length);
            if (value > 0) ups_data.set("battery.capacity.full", String((int)value));
        } else if (strcmp(path, "UPS.PowerSummary.WarningCapacityLimit") == 0) {
            double value = HIDParser::extractUsage(&usage, report_id, data, length);
            if (value > 0) ups_data.set("battery.charge.low", String((int)value));
        } else if (strcmp(path, "UPS.PowerSummary.DelayBeforeStartup") == 0) {
            double value = HIDParser::extractUsage(&usage, report_id, data, length);
            ups_data.set("ups.timer.start", String((int)value));
        } else if (strcmp(path, "UPS.PowerSummary.RunTimeToEmpty") == 0) {
            // GoldenMate firmware has been observed to return zero or about seven
            // days for a 296 Wh pack. Do not expose those as shutdown guidance.
            double value = HIDParser::extractUsage(&usage, report_id, data, length);
            if (value <= 0 || value > 86400) ups_data.remove("battery.runtime");
        }
    }
}
