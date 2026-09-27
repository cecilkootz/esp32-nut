#ifndef NUT_SERVER_H
#define NUT_SERVER_H

#include <Arduino.h>
#include <vector>
#include "IUSBHostUPS.h"

#ifndef PIO_UNIT_TESTING
#include <WiFi.h>
#endif

#define NUT_DEFAULT_PORT 3493
#define NUT_MAX_CLIENTS 4
// upsd drops clients idle for 60 s; this leaves room for once-a-minute pollers.
#define NUT_TIMEOUT_MS 120000

struct NUTServerConfig {
    String username;
    String password;
    String ups_name;
};

class NUTServer {
public:
    typedef void (*LogCallback)(const char* level, const char* msg);

    // Since boot, updated and read on the loop task only.
    struct Stats {
        uint32_t accepted = 0;
        uint32_t rejected = 0;      // every slot taken
        uint32_t commands = 0;
        uint32_t authFailures = 0;  // PASSWORD refused
        uint32_t idleTimeouts = 0;
        uint32_t shortWrites = 0;   // sessions dropped after a reply fell short
    };

    NUTServer();
    ~NUTServer();

    // Inizializza il server TCP e memorizza la configurazione e le dipendenze
    bool begin(const NUTServerConfig& config, IUSBHostUPS* usb_ups, uint16_t port = NUT_DEFAULT_PORT);
    
    // Esegue la gestione non bloccante delle connessioni e dei comandi
    void loop();

    // Without one, messages go to Serial.
    void setLogCallback(LogCallback cb) { _log_cb = cb; }

    uint16_t port() const { return _port; }
    int connectedClients() const;
    const Stats& stats() const { return _stats; }

    // Metodi di utilità (esposti per facilitare il testing)
    static std::vector<String> splitTokens(const String& input);
    void processCommand(Print& client, int slot, const String& cmdLine);

    // Helpers per testing
    void setAuthenticated(int slot, bool auth) { if (slot >= 0 && slot < NUT_MAX_CLIENTS) _clientAuthenticated[slot] = auth; }
    void setClientUsername(int slot, const String& user) { if (slot >= 0 && slot < NUT_MAX_CLIENTS) _clientUsername[slot] = user; }

private:
    void handleCommand(int slot, const String& cmdLine);
    void closeSession(int slot);
    bool upsAvailable(Print& client) const;
    void logMessage(const char* level, const char* format, ...) const __attribute__((format(printf, 3, 4)));

    NUTServerConfig _config;
    IUSBHostUPS* _usb_ups;
    uint16_t _port;
    bool _initialized;
    LogCallback _log_cb = nullptr;
    Stats _stats;

#ifndef PIO_UNIT_TESTING
    WiFiServer _server;
    WiFiClient _clients[NUT_MAX_CLIENTS];
#endif
    bool _clientActive[NUT_MAX_CLIENTS];
    bool _clientAuthenticated[NUT_MAX_CLIENTS];
    uint32_t _clientLastActivity[NUT_MAX_CLIENTS];
    String _clientBuffer[NUT_MAX_CLIENTS];
    String _clientUsername[NUT_MAX_CLIENTS];
};

#endif // NUT_SERVER_H
