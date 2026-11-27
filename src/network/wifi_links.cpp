#include "network/wifi_links.h"
#include "core/app_logger.h"
#include "NetHostname.h"
#include <esp_mac.h>

// The core retries a dropped link at once; this only nudges it for the reasons it
// gives up on, such as a password later corrected on the AP.
static const uint32_t RECONNECT_NUDGE_MS = 60000;

static const char* statusName(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
        case WL_CONNECTED:       return "WL_CONNECTED";
        case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED:    return "WL_DISCONNECTED";
        case WL_STOPPED:         return "WL_STOPPED";
        case WL_NO_SHIELD:       return "WL_NO_SHIELD";
    }
    return "UNKNOWN";
}

const char* WifiStaLink::reasonName(uint8_t reason) {
    const char* name = WiFi.disconnectReasonName((wifi_err_reason_t)reason);
    return *name ? name : "UNKNOWN";
}

WifiStaLink::WifiStaLink(const String& ssid, const String& password, const String& saved_hostname)
    : m_ssid(ssid),
      m_password(password),
      m_savedHostname(saved_hostname),
      m_lastStatus(WL_IDLE_STATUS),
      m_lastConnectionAttempt(0),
      m_eventHandler(0),
      m_disconnectCount(0),
      m_lastDisconnectReason(0),
      m_lastDisconnectTime(0),
      m_linkDown(false) {}

WifiStaLink::~WifiStaLink() {
    if (m_eventHandler) WiFi.removeEvent(m_eventHandler);
}

void WifiStaLink::onWiFiEvent(arduino_event_id_t event, const arduino_event_info_t& info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        // The core retries at once, so an outage repeats the same reason every
        // few seconds. Log it when the link drops and whenever the reason changes.
        if (!m_linkDown || reason != m_lastDisconnectReason) {
            AppLogger::log("WARN", "[NETWORK] Disconnected: reason %d (%s)", reason, reasonName(reason));
        }
        m_linkDown = true;
        m_lastDisconnectReason = reason;
        m_lastDisconnectTime = millis();
        m_disconnectCount++;
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        m_linkDown = false;
        AppLogger::log("INFO", "[NETWORK] Connection established! IP Address: %s",
                       IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    }
}

void WifiStaLink::begin() {
    m_lastStatus = WL_IDLE_STATUS;

    AppLogger::log("INFO", "[NETWORK] Initializing Wi-Fi...");
    // Only takes effect before the first WiFi.mode(). The credentials already
    // live in our own NVS namespace; left persistent, the driver would save its
    // config to NVS again on every WiFi.begin() and reconnect.
    WiFi.persistent(false);
    // Before WiFi.mode() when it is the first: the event task walks the handler
    // list without a lock, so adding to it later could race.
    if (!m_eventHandler) {
        auto handler = [this](arduino_event_id_t event, const arduino_event_info_t& info) {
            onWiFiEvent(event, info);
        };
        m_eventHandler = WiFi.onEvent(handler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        WiFi.onEvent(handler, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    }
    WiFi.mode(WIFI_STA);
    // Modem sleep can leave the node unreachable on some access points; on
    // mains/UPS power the saving isn't worth that.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true); // Consente all'ESP32 di gestire le riconnessioni a basso livello
    // The default fast scan joins the first AP that answers for the SSID and the
    // station never roams, so on a multi-AP network that choice would hold until
    // the link drops. WiFi.begin() copies both settings into the station config.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    // US-113: on the STA netif, after WiFi.mode() (which writes the core default name) and
    // before WiFi.begin(), so the first DHCP request carries it. WiFi.STA.setHostname()
    // takes up to 32 characters, WiFi.setHostname() only 31 (NetHostname.h)
    // MAC from eFuse: right after WiFi.mode() the STA netif has not started yet and
    // WiFi.macAddress() gives 00:00:00:00:00:00 (seen on the S3, core 3.3.12). Same value
    // as WiFi.macAddress() once the station runs, so name and device ID match
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) WiFi.macAddress(mac);
    m_hostname = NetHostname::effective(m_savedHostname, mac);
    if (WiFi.STA.setHostname(m_hostname.c_str())) {
        AppLogger::log("INFO", "[NETWORK] Wi-Fi hostname %s", m_hostname.c_str());
    } else {
        AppLogger::log("WARN", "[NETWORK] Could not set the Wi-Fi hostname %s", m_hostname.c_str());
    }

    AppLogger::log("INFO", "[NETWORK] Connecting to SSID: %s...\n", m_ssid.c_str());
    WiFi.begin(m_ssid.c_str(), m_password.c_str());
    m_lastConnectionAttempt = millis();
}

void WifiStaLink::end() {
    // Radio off too (US-127): with only disconnect() the automatic reconnection would
    // bring the station back while the board is on Ethernet
    WiFi.disconnect(true);
}

void WifiStaLink::loop(uint32_t now) {
    wl_status_t currentStatus = WiFi.status();

    // Rileva cambiamenti di stato
    if (currentStatus != m_lastStatus) {
        // Getting connected is logged by onWiFiEvent().
        if (currentStatus == WL_DISCONNECTED && m_lastStatus == WL_CONNECTED) {
            AppLogger::log("WARN", "[NETWORK] Wi-Fi connection lost!");
        } else if (currentStatus == WL_NO_SSID_AVAIL) {
            AppLogger::log("WARN", "[NETWORK] SSID network not found.");
        } else if (currentStatus == WL_CONNECT_FAILED) {
            AppLogger::log("ERROR", "[NETWORK] Connection failed.");
        } else if (currentStatus == WL_CONNECTION_LOST) {
            AppLogger::log("WARN", "[NETWORK] Connection lost (WL_CONNECTION_LOST).");
        }
        m_lastStatus = currentStatus;
    }

    if (currentStatus == WL_CONNECTED) {
        m_lastConnectionAttempt = now; // Reset attempt timer while connected
    }

    // The core already retries every transient disconnect at once, and a quicker
    // kick could cut off an association or DHCP still in progress.
    if (currentStatus != WL_CONNECTED) {
        if (now - m_lastConnectionAttempt >= RECONNECT_NUDGE_MS) {
            m_lastConnectionAttempt = now;
            AppLogger::log("WARN", "[NETWORK] Link still down (%s), asking the driver to reconnect", statusName(currentStatus));
            if (!WiFi.reconnect()) {
                AppLogger::log("WARN", "[NETWORK] WiFi.reconnect() failed");
            }
        }
    }
}

bool WifiStaLink::isConnected() const {
    return (WiFi.status() == WL_CONNECTED);
}

String WifiStaLink::statusText() const {
    return isConnected() ? WiFi.SSID() : String("Connecting");
}

String WifiStaLink::localIP() const {
    return WiFi.localIP().toString();
}

void WifiStaLink::macAddress(uint8_t mac[6]) const {
    WiFi.macAddress(mac);
}

void WifiStaLink::linkInfo(NetLinkInfo& out) const {
    out = NetLinkInfo();
    out.type = typeName();
    macAddress(out.mac);
    if (!isConnected()) {
        out.state = NetLinkState::CONNECTING;
        out.ssid = m_ssid; // the network it is joining
        return;
    }
    out.state = NetLinkState::CONNECTED;
    out.ip = WiFi.localIP().toString();
    out.gateway = WiFi.gatewayIP().toString();
    out.dhcp = true; // no static address on the station
    out.ssid = WiFi.SSID();
    out.rssi = WiFi.RSSI();
    out.has_rssi = true;
}

WifiApLink::WifiApLink(const String& ssid, const String& password)
    : m_ssid(ssid), m_password(password) {}

void WifiApLink::begin() {
    AppLogger::log("INFO", "[NETWORK] Starting Access Point mode...");
    // Only takes effect before the first WiFi.mode(). The AP settings are fixed
    // in firmware, so there is nothing for the driver to keep in flash.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(m_ssid.c_str(), m_password.c_str());
    AppLogger::log("INFO", "[NETWORK] AP Started. IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void WifiApLink::end() {
    WiFi.softAPdisconnect(true);
}

String WifiApLink::localIP() const {
    return WiFi.softAPIP().toString();
}

void WifiApLink::macAddress(uint8_t mac[6]) const {
    // Same MAC as before US-090: the device ID was always built from the station MAC
    WiFi.macAddress(mac);
}

void WifiApLink::linkInfo(NetLinkInfo& out) const {
    out = NetLinkInfo();
    out.type = typeName();
    out.state = NetLinkState::ACCESS_POINT;
    out.ssid = m_ssid; // the board's own network
    out.ip = localIP();
    macAddress(out.mac);
}

uint32_t WifiApLink::stationCount() const {
    return WiFi.softAPgetStationNum();
}
