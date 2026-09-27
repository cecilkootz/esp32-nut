#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <Arduino.h>

// Chip model and the last three bytes of the station MAC, e.g. "ESP32-S3-A1B2C3".
// Also a valid hostname.
String getDeviceId();

// Station MAC as "aa:bb:cc:dd:ee:ff". Like the ID, readable before Wi-Fi starts.
String getMacAddress();

#endif // DEVICE_ID_H
