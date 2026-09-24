#ifndef GOLDENMATE_DRIVER_H
#define GOLDENMATE_DRIVER_H

#include "GenericDriver.h"

// GoldenMate LiFePO4 models using the iDowell HID descriptor (075d:0300).
class GoldenMateDriver : public GenericDriver {
public:
    const char* getDriverName() const override { return "GoldenMateDriver"; }
    void decodeReport(IUSBHostUPS* host, uint8_t report_id, uint8_t report_type,
                      const uint8_t* data, size_t length, UPSData& ups_data) override;

private:
    uint8_t _ambiguousStatusReports = 0;
};

#endif
