#include "core/device_id.h"
#include <esp_mac.h>

String getDeviceId() {
    // Unlike WiFi.macAddress(), works before Wi-Fi is started and in AP mode.
    // The first three bytes are Espressif's OUI, so only the rest tell boards apart.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[32];
    snprintf(id, sizeof(id), "%s-%02X%02X%02X", ESP.getChipModel(), mac[3], mac[4], mac[5]);
    return String(id);
}
