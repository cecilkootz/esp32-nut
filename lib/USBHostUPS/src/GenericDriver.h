#ifndef GENERIC_DRIVER_H
#define GENERIC_DRIVER_H

#include "IUPSDriver.h"
#include <Arduino.h>
#include <vector>

class GenericDriver : public IUPSDriver {
public:
    const char* getDriverName() const override { return "GenericDriver"; }
public:
    GenericDriver();
    virtual ~GenericDriver() = default;

    void setup() override;
    void loop(IUSBHostUPS* host, UPSData& data, uint32_t now) override;
    void decodeReport(IUSBHostUPS* host, uint8_t report_id, uint8_t report_type, const uint8_t *data, size_t length, UPSData& ups_data) override;
    void parseStringDescriptor(IUSBHostUPS* host, uint8_t index, const uint8_t *data, size_t length, UPSData& ups_data) override;

    // driver.name and driver.version identify this firmware, as a NUT driver
    // identifies itself; driver.version.data names the sub-driver. Every
    // loop() override calls this.
    void publishDriverInfo(UPSData& data) const;

protected:
    // Reports worth polling, as (type << 8) | id in descriptor order: those
    // shouldPoll() accepts, less Output reports and Input reports that share
    // an ID with a Feature report. The usage table is fixed per connection, so
    // this is built on first use after setup(), which the host calls for every
    // new descriptor.
    const std::vector<uint16_t>& pollList(IUSBHostUPS* host);
    virtual bool shouldPoll(uint8_t report_id, uint8_t report_type) const;

    uint32_t _last_poll;
    uint32_t _last_fast_poll;
    uint32_t _last_step_time;
    uint8_t _poll_step;
    uint8_t _slow_poll_counter;
    String _active_beeper;
    uint8_t _batteryDateStringIndex;

private:
    std::vector<uint16_t> _poll_list;
    bool _poll_list_ready = false;
};

#endif // GENERIC_DRIVER_H
