#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>

class AppNetworkManager {
public:
    AppNetworkManager();
    
    // Avvia la connessione Wi-Fi
    void begin(const String& ssid, const String& password);
    void beginAP(const String& ap_ssid, const String& ap_password);
    
    // Gestisce il monitoraggio e la riconnessione
    void loop();
    
    // Ritorna true se la connessione è attiva
    bool isConnected() const;

    // Station disconnect events since boot. The lastDisconnect* values mean
    // nothing while the count is 0.
    uint32_t disconnectCount() const;
    uint8_t lastDisconnectReason() const;
    const char* lastDisconnectReasonName() const;
    uint32_t lastDisconnectMillis() const;

private:
    void onWiFiEvent(arduino_event_id_t event, const arduino_event_info_t& info);

    String m_ssid;
    String m_password;
    wl_status_t m_lastStatus;
    uint32_t m_lastConnectionAttempt;
    bool m_isStarted;
    bool m_isApMode;
    bool m_eventsRegistered;

    // Written by onWiFiEvent() on the Arduino network event task.
    std::atomic<uint32_t> m_disconnectCount;
    std::atomic<uint8_t> m_lastDisconnectReason;
    std::atomic<uint32_t> m_lastDisconnectTime;
    bool m_linkDown; // only touched by onWiFiEvent()
};

#endif // NETWORK_MANAGER_H
