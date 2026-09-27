#include "network/network_manager.h"
#include "core/app_logger.h"

AppNetworkManager::AppNetworkManager() 
    : m_lastStatus(WL_IDLE_STATUS), 
      m_lastConnectionAttempt(0), 
      m_lastDisconnectTime(0),
      m_isStarted(false),
      m_isApMode(false) {}

void AppNetworkManager::beginAP(const String& ap_ssid, const String& ap_password) {
    m_isStarted = true;
    m_isApMode = true;
    AppLogger::log("INFO", "[NETWORK] Starting Access Point mode...");
    // Only takes effect before the first WiFi.mode(). The AP settings are fixed
    // in firmware, so there is nothing for the driver to keep in flash.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());
    AppLogger::log("INFO", "[NETWORK] AP Started. IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void AppNetworkManager::begin(const String& ssid, const String& password) {
    m_ssid = ssid;
    m_password = password;
    m_isStarted = true;
    m_isApMode = false;
    m_lastStatus = WL_IDLE_STATUS;
    
    AppLogger::log("INFO", "[NETWORK] Initializing Wi-Fi...");
    // Only takes effect before the first WiFi.mode(). The credentials already
    // live in our own NVS namespace; left persistent, the driver would save its
    // config to NVS again on every WiFi.begin() and reconnect.
    WiFi.persistent(false);
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
    
    AppLogger::log("INFO", "[NETWORK] Connecting to SSID: %s...\n", m_ssid.c_str());
    WiFi.begin(m_ssid.c_str(), m_password.c_str());
    m_lastConnectionAttempt = millis();
    m_lastDisconnectTime = m_lastConnectionAttempt;
}

void AppNetworkManager::loop() {
    if (!m_isStarted || m_isApMode) {
        return;
    }
    
    wl_status_t currentStatus = WiFi.status();
    uint32_t now = millis();
    
    // Rileva cambiamenti di stato
    if (currentStatus != m_lastStatus) {
        if (m_lastStatus == WL_CONNECTED && currentStatus != WL_CONNECTED) {
            m_lastDisconnectTime = now;
        }
        
        if (currentStatus == WL_CONNECTED) {
            AppLogger::log("INFO", "[NETWORK] Connection established! IP Address: %s\n", WiFi.localIP().toString().c_str());
        } else if (currentStatus == WL_DISCONNECTED && m_lastStatus == WL_CONNECTED) {
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
    
    // Gestione riconnessione e fallback
    if (currentStatus != WL_CONNECTED) {
        if (now - m_lastConnectionAttempt >= 15000) {
            m_lastConnectionAttempt = now;
            AppLogger::log("INFO", "[NETWORK] Reconnecting... Attempt on SSID: %s\n", m_ssid.c_str());
            // Forza una nuova connessione
            WiFi.disconnect();
            WiFi.begin(m_ssid.c_str(), m_password.c_str());
        }
    }
}

bool AppNetworkManager::isConnected() const {
    return (WiFi.status() == WL_CONNECTED);
}
