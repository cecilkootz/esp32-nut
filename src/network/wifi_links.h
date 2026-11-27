#ifndef WIFI_LINKS_H
#define WIFI_LINKS_H

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include "INetLink.h"

// Wi-Fi adapters of the net module (ADR 0010): the only code that calls WiFi.*

// Station on the home network, with the reconnection of the former AppNetworkManager
class WifiStaLink : public INetLink {
public:
    // saved_hostname: the name chosen in the Network page, "" for the default (US-113)
    WifiStaLink(const String& ssid, const String& password, const String& saved_hostname);
    ~WifiStaLink() override;

    NetLinkKind kind() const override { return NetLinkKind::STATION; }
    void begin() override;
    void end() override;
    void loop(uint32_t now) override;
    bool isConnected() const override;
    String statusText() const override;
    String localIP() const override;
    void macAddress(uint8_t mac[6]) const override;
    void linkInfo(NetLinkInfo& out) const override;
    String hostname() const override { return m_hostname; }

    // Station disconnect events since boot. The lastDisconnect* values mean
    // nothing while the count is 0.
    uint32_t disconnectCount() const { return m_disconnectCount; }
    uint8_t lastDisconnectReason() const { return m_lastDisconnectReason; }
    uint32_t lastDisconnectMillis() const { return m_lastDisconnectTime; }
    static const char* reasonName(uint8_t reason);

private:
    void onWiFiEvent(arduino_event_id_t event, const arduino_event_info_t& info);

    String m_ssid;
    String m_password;
    String m_savedHostname;
    String m_hostname;
    wl_status_t m_lastStatus;
    uint32_t m_lastConnectionAttempt;
    wifi_event_id_t m_eventHandler;

    // Written by onWiFiEvent() on the Arduino network event task.
    std::atomic<uint32_t> m_disconnectCount;
    std::atomic<uint8_t> m_lastDisconnectReason;
    std::atomic<uint32_t> m_lastDisconnectTime;
    bool m_linkDown; // only touched by onWiFiEvent()
};

// Configuration access point (first setup, double reset), with the captive portal
class WifiApLink : public INetLink {
public:
    WifiApLink(const String& ssid, const String& password);

    NetLinkKind kind() const override { return NetLinkKind::ACCESS_POINT; }
    void begin() override;
    void end() override;
    void loop(uint32_t) override {}
    // The status LED shows the AP mode instead (computeSystemState)
    bool isConnected() const override { return false; }
    String statusText() const override { return "AP Mode Active"; }
    String localIP() const override;
    void macAddress(uint8_t mac[6]) const override;
    void linkInfo(NetLinkInfo& out) const override;
    uint32_t stationCount() const;

private:
    String m_ssid;
    String m_password;
};

#endif // WIFI_LINKS_H
