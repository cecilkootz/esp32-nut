#include "NUTServer.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef PIO_UNIT_TESTING
#include <errno.h>
#include <lwip/sockets.h>
#endif

// Shared by LIST UPS and GET UPSDESC so the two cannot drift.
static const char* NUT_UPS_DESCRIPTION = "ESP32-S3 UPS Bridge";

// Injected at build time from the release tag; "dev" for local builds, matching
// src/network/web_config_server.cpp.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// Protocol level the implemented command subset targets, as upsd reports it.
static const char* NUT_PROTOCOL_VERSION = "1.3";

// Older readings are refused. CyberPower, the slowest driver, polls every
// 30 s, so this rides out two missed polls.
static const uint32_t NUT_DATA_MAX_AGE_MS = 90000;

namespace {

// Replies are assembled here and written once, after the USB data lock is
// released: NetworkClient::write blocks for up to 10 s per call when the peer
// stops reading, and the USB task waits on that same lock.
class ReplyBuffer : public Print {
public:
    String text;

    size_t write(uint8_t c) override {
        return text.concat((char)c) ? 1 : 0;
    }

    size_t write(const uint8_t* buf, size_t size) override {
        return text.concat((const char*)buf, size) ? size : 0;
    }
};

// Escapes quotes and backslashes in the value as upsd does. Left bare, a quote
// ends the value early and a trailing backslash swallows the closing quote.
void printVar(Print& out, const String& ups, const char* name, const String& value) {
    out.printf("VAR %s %s \"", ups.c_str(), name);
    const char* run = value.c_str();
    if (!run) run = "";
    while (*run) {
        size_t plain = strcspn(run, "\"\\");
        if (plain) out.write((const uint8_t*)run, plain);
        run += plain;
        if (*run) {
            const uint8_t escaped[2] = {'\\', (uint8_t)*run++};
            out.write(escaped, sizeof(escaped));
        }
    }
    out.print("\"\n");
}

#ifndef PIO_UNIT_TESTING
// Stops forwarding after a short write, which NetworkClient returns after 10 s
// without progress or once the connection has failed.
class ClientWriter : public Print {
public:
    explicit ClientWriter(Print& client) : _client(client) {}

    bool shortWrite = false;

    size_t write(uint8_t c) override {
        return write(&c, 1);
    }

    size_t write(const uint8_t* buf, size_t size) override {
        if (shortWrite) return 0;
        size_t sent = _client.write(buf, size);
        if (sent < size) shortWrite = true;
        return sent;
    }

private:
    Print& _client;
};

// The core enables SO_KEEPALIVE on accepted sockets, but LwIP's defaults send
// the first probe after two hours. With these, LwIP aborts the connection once
// the peer has been unreachable for a minute, and connected() then reports it.
bool enableKeepalive(WiFiClient& client) {
    const int idle_s = 30;
    const int interval_s = 10;
    const int probes = 3;
    return client.setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof(idle_s)) == 0 &&
           client.setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &interval_s, sizeof(interval_s)) == 0 &&
           client.setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) == 0;
}
#endif

}  // namespace

NUTServer::NUTServer() : 
    _usb_ups(nullptr), 
    _port(NUT_DEFAULT_PORT), 
    _initialized(false)
#ifndef PIO_UNIT_TESTING
    , _server(NUT_DEFAULT_PORT)
#endif
{
    for (int i = 0; i < NUT_MAX_CLIENTS; i++) {
        _clientActive[i] = false;
        _clientAuthenticated[i] = false;
        _clientLastActivity[i] = 0;
        _clientBuffer[i] = "";
        _clientUsername[i] = "";
    }
}

NUTServer::~NUTServer() {
#ifndef PIO_UNIT_TESTING
    for (int i = 0; i < NUT_MAX_CLIENTS; i++) {
        if (_clientActive[i]) {
            _clients[i].stop();
        }
    }
#endif
}

bool NUTServer::begin(const NUTServerConfig& config, IUSBHostUPS* usb_ups, uint16_t port) {
    _config = config;
    _usb_ups = usb_ups;
    _port = port;

#ifndef PIO_UNIT_TESTING
    _server = WiFiServer(_port);
    _server.begin();
#endif
    
    _initialized = true;
    logMessage("INFO", "[NUTServer] Server listening on port %d", _port);
    return true;
}

void NUTServer::logMessage(const char* level, const char* format, ...) const {
    char msg[160];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    if (_log_cb) {
        _log_cb(level, msg);
    } else {
        Serial.printf("%s\n", msg);
    }
}

// Mirrors upsd's ups_available(): a read fails outright rather than serve
// readings the UPS is no longer backing.
bool NUTServer::upsAvailable(Print& client) const {
    if (!_usb_ups || !_usb_ups->isConnected()) {
        client.print("ERR DRIVER-NOT-CONNECTED\n");
        return false;
    }
    if (!_usb_ups->hasFreshData(NUT_DATA_MAX_AGE_MS)) {
        client.print("ERR DATA-STALE\n");
        return false;
    }
    return true;
}

int NUTServer::connectedClients() const {
    int count = 0;
    for (int i = 0; i < NUT_MAX_CLIENTS; i++) {
        if (_clientActive[i]) count++;
    }
    return count;
}

void NUTServer::closeSession(int slot) {
    if (slot >= 0 && slot < NUT_MAX_CLIENTS) {
#ifndef PIO_UNIT_TESTING
        if (_clients[slot]) {
            _clients[slot].stop();
        }
#endif
        _clientActive[slot] = false;
        _clientAuthenticated[slot] = false;
        _clientLastActivity[slot] = 0;
        _clientBuffer[slot] = "";
        _clientUsername[slot] = "";
        logMessage("INFO", "[NUTServer] Session for slot %d closed.", slot);
    }
}

std::vector<String> NUTServer::splitTokens(const String& input) {
    std::vector<String> tokens;
    String currentToken = "";
    bool inQuotes = false;
    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == ' ' && !inQuotes) {
            if (currentToken.length() > 0) {
                tokens.push_back(currentToken);
                currentToken = "";
            }
        } else {
            currentToken += c;
        }
    }
    if (currentToken.length() > 0) {
        tokens.push_back(currentToken);
    }
    return tokens;
}

void NUTServer::loop() {
    if (!_initialized) {
        return;
    }

#ifndef PIO_UNIT_TESTING
    // 1. Accetta nuove connessioni client
    if (_server.hasClient()) {
        WiFiClient newClient = _server.accept();
        if (newClient) {
            int slot = -1;
            for (int i = 0; i < NUT_MAX_CLIENTS; i++) {
                if (!_clientActive[i] || !_clients[i].connected()) {
                    slot = i;
                    break;
                }
            }

            if (slot != -1) {
                if (_clients[slot]) {
                    _clients[slot].stop();
                }
                _clients[slot] = newClient;
                if (!enableKeepalive(_clients[slot])) {
                    logMessage("WARN", "[NUTServer] Keepalive setup failed for slot %d (errno %d).", slot, errno);
                }

                _clientActive[slot] = true;
                _clientAuthenticated[slot] = false;
                _clientLastActivity[slot] = millis();
                _clientBuffer[slot] = "";
                _clientBuffer[slot].reserve(256);
                _clientUsername[slot] = "";
                _stats.accepted++;
                logMessage("INFO", "[NUTServer] Client connected to slot %d from %s:%d",
                           slot, newClient.remoteIP().toString().c_str(), newClient.remotePort());
            } else {
                _stats.rejected++;
                logMessage("WARN", "[NUTServer] Connection rejected: max clients reached");
                newClient.print("ERR FAILED - Max clients reached\n");
                newClient.stop();
            }
        }
    }

    // 2. Gestisci i client attivi
    for (int i = 0; i < NUT_MAX_CLIENTS; i++) {
        if (_clientActive[i]) {
            if (!_clients[i].connected() && !_clients[i].available()) {
                closeSession(i);
                continue;
            }

            if (millis() - _clientLastActivity[i] > NUT_TIMEOUT_MS) {
                _stats.idleTimeouts++;
                logMessage("INFO", "[NUTServer] Inactivity timeout for slot %d. Disconnecting.", i);
                _clients[i].print("ERR ACCESS-DENIED\n");
                closeSession(i);
                continue;
            }

            // Leggi dati disponibili dal buffer del client
            for (int b = 0; b < 128 && _clientActive[i] && _clients[i].available(); b++) {
                char c = _clients[i].read();
                _clientLastActivity[i] = millis(); // Resetta il timer di inattività
                
                if (c == '\n' || c == '\r') {
                    if (_clientBuffer[i].length() > 0) {
                        handleCommand(i, _clientBuffer[i]);
                        _clientBuffer[i] = "";
                    }
                } else if (c >= 32 && c <= 126) {
                    if (_clientBuffer[i].length() < 256) {
                        _clientBuffer[i] += c;
                    }
                }
            }
        }
    }
#endif
}

void NUTServer::handleCommand(int slot, const String& cmdLine) {
#ifndef PIO_UNIT_TESTING
    ClientWriter client(_clients[slot]);
    processCommand(client, slot, cmdLine);
    // Close now: each command the peer queued before it stopped reading would
    // cost another 10 s. LOGOUT has already closed the session itself.
    if (client.shortWrite && _clientActive[slot]) {
        _stats.shortWrites++;
        logMessage("WARN", "[NUTServer] Write to slot %d fell short. Disconnecting.", slot);
        closeSession(slot);
    }
#endif
}

void NUTServer::processCommand(Print& client, int slot, const String& cmdLine) {
    std::vector<String> tokens = splitTokens(cmdLine);
    if (tokens.empty()) {
        return;
    }
    _stats.commands++;

    String cmd = tokens[0];
    cmd.toUpperCase();

    bool authRequired = (_config.username.length() > 0 && _config.password.length() > 0);

    if (cmd == "VER") {
        client.printf("Network UPS Tools esp32-nut %s\n", FIRMWARE_VERSION);
        return;
    }

    if (cmd == "NETVER") {
        client.printf("%s\n", NUT_PROTOCOL_VERSION);
        return;
    }

    if (cmd == "USERNAME") {
        if (tokens.size() < 2) {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
        _clientUsername[slot] = tokens[1];
        client.print("OK\n");
        return;
    }

    if (cmd == "PASSWORD") {
        if (tokens.size() < 2) {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
        String password = tokens[1];
        if (_clientUsername[slot] == _config.username && password == _config.password) {
            _clientAuthenticated[slot] = true;
            client.print("OK\n");
        } else {
            _stats.authFailures++;
            client.print("ERR ACCESS-DENIED\n");
        }
        return;
    }

    if (cmd == "LOGIN") {
        if (tokens.size() < 2) {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
        String upsName = tokens[1];
        String upsNameLower = upsName;
        upsNameLower.toLowerCase();
        String configUpsNameLower = _config.ups_name;
        configUpsNameLower.toLowerCase();

        if (upsNameLower != configUpsNameLower) {
            client.print("ERR UNKNOWN-UPS\n");
            return;
        }

        if (authRequired && !_clientAuthenticated[slot]) {
            client.print("ERR ACCESS-DENIED\n");
            return;
        }

        client.print("OK\n");
        return;
    }

    if (cmd == "LOGOUT") {
        client.print("OK Goodbye\n");
        closeSession(slot);
        return;
    }

    if (cmd == "LIST") {
        if (tokens.size() < 2) {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
        String subcmd = tokens[1];
        subcmd.toUpperCase();

        if (subcmd == "UPS") {
            ReplyBuffer reply;
            reply.print("BEGIN LIST UPS\n");
            reply.printf("UPS %s \"%s\"\n", _config.ups_name.c_str(), NUT_UPS_DESCRIPTION);
            reply.print("END LIST UPS\n");
            client.print(reply.text);
            return;
        } 
        else if (subcmd == "VAR") {
            String upsName;
            if (tokens.size() < 3) {
                upsName = _config.ups_name;
            } else {
                upsName = tokens[2];
            }
            String upsNameLower = upsName;
            upsNameLower.toLowerCase();
            String configUpsNameLower = _config.ups_name;
            configUpsNameLower.toLowerCase();
            if (upsNameLower != configUpsNameLower) {
                client.print("ERR UNKNOWN-UPS\n");
                return;
            }
            if (!upsAvailable(client)) {
                return;
            }

            ReplyBuffer reply;
            reply.printf("BEGIN LIST VAR %s\n", upsName.c_str());
            {
                auto data = _usb_ups->getUPSData();
                // String grows only to fit each append, so size it for every line up front.
                reply.text.reserve((data->getAll().size() + 3) * (upsName.length() + 48));
                printVar(reply, upsName, "ups.status", _usb_ups->getUPSStatusString());
                for (const auto& param : data->getAll()) {
                    if (param.key.startsWith("ups.status.") && param.key != "ups.status") continue;
                    printVar(reply, upsName, param.key.c_str(), param.value);
                }
            }
            reply.printf("END LIST VAR %s\n", upsName.c_str());
            client.print(reply.text);
            return;
        }
        else if (subcmd == "CMD" || subcmd == "RW" || subcmd == "CLIENT") {
            String upsName;
            if (tokens.size() < 3) {
                upsName = _config.ups_name;
            } else {
                upsName = tokens[2];
            }
            ReplyBuffer reply;
            reply.printf("BEGIN LIST %s %s\n", subcmd.c_str(), upsName.c_str());
            if (subcmd == "CMD" && _usb_ups) {
                if (_usb_ups->getUPSData()->hasKey("ups.beeper.status")) {
                    reply.printf("CMD %s beeper.enable\n", upsName.c_str());
                    reply.printf("CMD %s beeper.disable\n", upsName.c_str());
                    reply.printf("CMD %s beeper.toggle\n", upsName.c_str());
                }
            }
            reply.printf("END LIST %s %s\n", subcmd.c_str(), upsName.c_str());
            client.print(reply.text);
            return;
        }
        else if (subcmd == "ENUM" || subcmd == "RANGE") {
            String upsName;
            String varName;
            if (tokens.size() < 3) {
                client.print("ERR INVALID-ARGUMENT\n");
                return;
            } else if (tokens.size() == 3) {
                upsName = _config.ups_name;
                varName = tokens[2];
            } else {
                upsName = tokens[2];
                varName = tokens[3];
            }
            ReplyBuffer reply;
            reply.printf("BEGIN LIST %s %s %s\n", subcmd.c_str(), upsName.c_str(), varName.c_str());
            reply.printf("END LIST %s %s %s\n", subcmd.c_str(), upsName.c_str(), varName.c_str());
            client.print(reply.text);
            return;
        }
        else {
            client.print("ERR UNKNOWN-COMMAND\n");
            return;
        }
    }

    if (cmd == "INSTCMD") {
        if (authRequired && !_clientAuthenticated[slot]) {
            client.print("ERR ACCESS-DENIED\n");
            return;
        }
        if (tokens.size() < 3) {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
        String upsName = tokens[1];
        String cmdName = tokens[2];
        
        String upsNameLower = upsName;
        upsNameLower.toLowerCase();
        String configUpsNameLower = _config.ups_name;
        configUpsNameLower.toLowerCase();
        if (upsNameLower != configUpsNameLower) {
            client.print("ERR UNKNOWN-UPS\n");
            return;
        }

        if (cmdName == "beeper.enable") {
            if (!_usb_ups || !_usb_ups->getUPSData()->hasKey("ups.beeper.status")) {
                client.print("ERR CMD-NOT-SUPPORTED\n");
            } else {
                _usb_ups->setBeeper(true);
                client.print("OK\n");
            }
            return;
        } else if (cmdName == "beeper.disable") {
            if (!_usb_ups || !_usb_ups->getUPSData()->hasKey("ups.beeper.status")) {
                client.print("ERR CMD-NOT-SUPPORTED\n");
            } else {
                _usb_ups->setBeeper(false);
                client.print("OK\n");
            }
            return;
        } else if (cmdName == "beeper.toggle") {
            if (!_usb_ups || !_usb_ups->getUPSData()->hasKey("ups.beeper.status")) {
                client.print("ERR CMD-NOT-SUPPORTED\n");
            } else {
                _usb_ups->setBeeper(!_usb_ups->getUPSData()->getBool("ups.beeper.status"));
                client.print("OK\n");
            }
            return;
        }
        client.print("ERR CMD-NOT-SUPPORTED\n");
        return;
    }

    if (cmd == "GET") {
        if (tokens.size() < 2) {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
        String subcmd = tokens[1];
        subcmd.toUpperCase();

        if (subcmd == "VAR") {
            String upsName;
            String varName;
            if (tokens.size() < 3) {
                client.print("ERR INVALID-ARGUMENT\n");
                return;
            } else if (tokens.size() == 3) {
                upsName = _config.ups_name;
                varName = tokens[2];
            } else {
                upsName = tokens[2];
                varName = tokens[3];
            }

            String upsNameLower = upsName;
            upsNameLower.toLowerCase();
            String configUpsNameLower = _config.ups_name;
            configUpsNameLower.toLowerCase();
            if (upsNameLower != configUpsNameLower) {
                client.print("ERR UNKNOWN-UPS\n");
                return;
            }
            if (!upsAvailable(client)) {
                return;
            }

            String varNameLower = varName;
            varNameLower.toLowerCase();

            ReplyBuffer reply;
            {
                auto data = _usb_ups->getUPSData();
                if (varNameLower == "ups.status") {
                    printVar(reply, upsName, "ups.status", _usb_ups->getUPSStatusString());
                } else if (data->hasKey(varNameLower)) {
                    printVar(reply, upsName, varNameLower.c_str(), data->get(varNameLower));
                } else {
                    reply.print("ERR VAR-NOT-SUPPORTED\n");
                }
            }
            client.print(reply.text);
            return;
        }
        else if (subcmd == "DESC" || subcmd == "CMDDESC" || subcmd == "TYPE") {
            String upsName;
            String varName;
            if (tokens.size() < 3) {
                client.print("ERR INVALID-ARGUMENT\n");
                return;
            } else if (tokens.size() == 3) {
                upsName = _config.ups_name;
                varName = tokens[2];
            } else {
                upsName = tokens[2];
                varName = tokens[3];
            }

            String upsNameLower = upsName;
            upsNameLower.toLowerCase();
            String configUpsNameLower = _config.ups_name;
            configUpsNameLower.toLowerCase();
            if (upsNameLower != configUpsNameLower) {
                client.print("ERR UNKNOWN-UPS\n");
                return;
            }

            if (subcmd == "TYPE") {
                // Every variable the bridge exposes is read-only (LIST RW is
                // empty) and goes out as a quoted string. Never answer a bare
                // "RW": clients read the token after it as the type.
                client.printf("TYPE %s %s STRING:64\n", upsName.c_str(), varName.c_str());
            } else {
                // DESC and CMDDESC share a shape. upsd answers "Unavailable"
                // when no description database is installed; this bridge
                // carries none for either variables or commands.
                client.printf("%s %s %s \"Unavailable\"\n", subcmd.c_str(),
                              upsName.c_str(), varName.c_str());
            }
            return;
        }
        else if (subcmd == "UPSDESC" || subcmd == "NUMLOGINS") {
            String upsName;
            if (tokens.size() < 3) {
                upsName = _config.ups_name;
            } else {
                upsName = tokens[2];
            }

            String upsNameLower = upsName;
            upsNameLower.toLowerCase();
            String configUpsNameLower = _config.ups_name;
            configUpsNameLower.toLowerCase();
            if (upsNameLower != configUpsNameLower) {
                client.print("ERR UNKNOWN-UPS\n");
                return;
            }

            if (subcmd == "UPSDESC") {
                client.printf("UPSDESC %s \"%s\"\n", upsName.c_str(), NUT_UPS_DESCRIPTION);
            } else {
                // This bridge exposes telemetry only; it tracks no upsmon LOGIN
                // sessions, so the count of logged-in clients is always zero.
                client.printf("NUMLOGINS %s 0\n", upsName.c_str());
            }
            return;
        }
        else {
            client.print("ERR INVALID-ARGUMENT\n");
            return;
        }
    }

    client.print("ERR UNKNOWN-COMMAND\n");
}



