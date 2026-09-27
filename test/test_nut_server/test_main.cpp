#include <unity.h>
#include "NUTServer.h"
#include "IUSBHostUPS.h"
#include "CyberPowerDriver.h"
#include <sstream>
#include <algorithm>

// Mock per catturare l'output di Print
class MemoryPrinter : public Print {
public:
    std::string buffer;

    size_t write(uint8_t c) override {
        buffer += (char)c;
        return 1;
    }

    size_t write(const uint8_t *b, size_t size) override {
        if (b && size > 0) {
            buffer.append((const char*)b, size);
        }
        return size;
    }

    void clear() {
        buffer.clear();
    }

    std::string getOutput() const {
        return buffer;
    }
};

// Mock di IUSBHostUPS
class MockUSBHost : public IUSBHostUPS {
public:
    UPSData data;
    String statusString = "OL";
    bool beeperState = true;
    bool connected = true;

    void end() override {}

    mutable int lockDepth = 0;
    void lock() const override { lockDepth++; }
    void unlock() const override { lockDepth--; }
    UPSDataLock getUPSData() const override {
        return UPSDataLock(data, this);
    }

    String getUPSStatusString() const override {
        return statusString;
    }

    bool setBeeper(bool enable) override {
        beeperState = enable;
        data.set("ups.beeper.status", enable ? "enabled" : "disabled");
        return true;
    }

    bool isConnected() const override {
        return connected;
    }

    bool fresh = true;
    mutable uint32_t lastMaxAge = 0;
    bool hasFreshData(uint32_t max_age_ms) const override {
        lastMaxAge = max_age_ms;
        return connected && fresh;
    }

    std::vector<HIDUsageDef> _mockUsages;
    HIDParser _hid_parser;
    const HIDParser* getHIDParser() const override { return &_hid_parser; }
    const std::vector<HIDUsageDef>& getUsages() const override { return _mockUsages; }
    const HIDUsageDef* getUsageDef(uint32_t) const override { return nullptr; }
    String getActiveBeeperPath() const override { return "UPS.PowerSummary.AudibleAlarmControl"; }
    uint32_t getQuirks() const override { return 0; }
    bool isControlPending() const override { return false; }
    bool requestReport(uint8_t, uint8_t, uint16_t) override { return true; }
    bool requestStringDescriptor(uint8_t) override { return true; }
};

static NUTServer server;
static MockUSBHost mockHost;
static MemoryPrinter printer;

// Each write() stands for one NetworkClient::write, which blocks for up to 10 s
// once the peer stops reading.
class SocketPrinter : public MemoryPrinter {
public:
    int writes = 0;
    int writesUnderLock = 0;

    size_t write(uint8_t c) override {
        record();
        return MemoryPrinter::write(c);
    }

    size_t write(const uint8_t *b, size_t size) override {
        record();
        return MemoryPrinter::write(b, size);
    }

private:
    void record() {
        writes++;
        if (mockHost.lockDepth > 0) writesUnderLock++;
    }
};

static std::vector<std::pair<std::string, std::string>> logged;

static void captureLog(const char* level, const char* msg) {
    logged.push_back({level, msg});
}

void setUp(void) {
    printer.clear();
    logged.clear();
    mockHost.data = UPSData();
    mockHost.statusString = "OL";
    mockHost.beeperState = true;
    mockHost.connected = true;
    mockHost.fresh = true;
    mockHost.lastMaxAge = 0;
    mockHost.lockDepth = 0;

    NUTServerConfig config;
    config.username = "admin";
    config.password = "secret";
    config.ups_name = "testups";

    // Sessions, counters and the log callback must not leak between tests.
    server = NUTServer();
    server.begin(config, &mockHost, 3493);
}

void tearDown(void) {}

void test_split_tokens(void) {
    std::vector<String> tokens1 = NUTServer::splitTokens("GET VAR testups battery.charge");
    TEST_ASSERT_EQUAL(4, tokens1.size());
    TEST_ASSERT_EQUAL_STRING("GET", tokens1[0].c_str());
    TEST_ASSERT_EQUAL_STRING("VAR", tokens1[1].c_str());
    TEST_ASSERT_EQUAL_STRING("testups", tokens1[2].c_str());
    TEST_ASSERT_EQUAL_STRING("battery.charge", tokens1[3].c_str());

    std::vector<String> tokens2 = NUTServer::splitTokens("GET VAR testups \"battery.charge\"");
    TEST_ASSERT_EQUAL(4, tokens2.size());
    TEST_ASSERT_EQUAL_STRING("battery.charge", tokens2[3].c_str());
}

void test_auth_flow(void) {
    // 1. Username
    server.processCommand(printer, 0, "USERNAME admin");
    TEST_ASSERT_EQUAL_STRING("OK\n", printer.getOutput().c_str());
    printer.clear();

    // 2. Wrong Password
    server.processCommand(printer, 0, "PASSWORD wrong");
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", printer.getOutput().c_str());
    printer.clear();

    // 3. Correct Password
    server.processCommand(printer, 0, "PASSWORD secret");
    TEST_ASSERT_EQUAL_STRING("OK\n", printer.getOutput().c_str());
    printer.clear();

    // 4. Login
    server.processCommand(printer, 0, "LOGIN testups");
    TEST_ASSERT_EQUAL_STRING("OK\n", printer.getOutput().c_str());
    printer.clear();

    // 5. Login wrong ups
    server.processCommand(printer, 0, "LOGIN wrongups");
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", printer.getOutput().c_str());
    printer.clear();
}

void test_list_ups(void) {
    server.processCommand(printer, 0, "LIST UPS");
    std::string out = printer.getOutput();
    TEST_ASSERT_TRUE(out.find("BEGIN LIST UPS\n") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("UPS testups \"ESP32-S3 UPS Bridge\"\n") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("END LIST UPS\n") != std::string::npos);
}

void test_get_var_compliance(void) {
    server.setAuthenticated(0, true);

    mockHost.data.set("battery.voltage", "13.6");
    mockHost.data.set("battery.temperature", "28.5");
    mockHost.data.set("battery.charge", "95");
    mockHost.data.set("battery.capacity", "100");
    mockHost.data.set("battery.capacity.full", "100");
    mockHost.data.set("battery.mfr.date", "2024/05/23");
    mockHost.data.set("ups.mfr.date", "2006/09/15");
    mockHost.data.set("battery.date", "2025/01/10");
    mockHost.data.set("output.voltage", "230.0");
    mockHost.data.set("ups.mfr", "APC");

    // Supported var: battery.voltage
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups battery.voltage");
    TEST_ASSERT_EQUAL_STRING("VAR testups battery.voltage \"13.6\"\n", printer.getOutput().c_str());

    // Supported var: battery.mfr.date
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups battery.mfr.date");
    TEST_ASSERT_EQUAL_STRING("VAR testups battery.mfr.date \"2024/05/23\"\n", printer.getOutput().c_str());

    // Supported var: ups.mfr.date
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups ups.mfr.date");
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.mfr.date \"2006/09/15\"\n", printer.getOutput().c_str());

    // Supported var: battery.date
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups battery.date");
    TEST_ASSERT_EQUAL_STRING("VAR testups battery.date \"2025/01/10\"\n", printer.getOutput().c_str());

    // Supported var: battery.temperature
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups battery.temperature");
    TEST_ASSERT_EQUAL_STRING("VAR testups battery.temperature \"28.5\"\n", printer.getOutput().c_str());

    // Supported var: battery.charge
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups battery.charge");
    TEST_ASSERT_EQUAL_STRING("VAR testups battery.charge \"95\"\n", printer.getOutput().c_str());

    // Supported var: battery.capacity.full
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups battery.capacity.full");
    TEST_ASSERT_EQUAL_STRING("VAR testups battery.capacity.full \"100\"\n", printer.getOutput().c_str());

    // Supported var: output.voltage
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups output.voltage");
    TEST_ASSERT_EQUAL_STRING("VAR testups output.voltage \"230.0\"\n", printer.getOutput().c_str());

    // Supported var: ups.mfr
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups ups.mfr");
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.mfr \"APC\"\n", printer.getOutput().c_str());

    // Unsupported var (has flag is false by default, e.g. input.voltage)
    printer.clear();
    server.processCommand(printer, 0, "GET VAR testups input.voltage");
    TEST_ASSERT_EQUAL_STRING("ERR VAR-NOT-SUPPORTED\n", printer.getOutput().c_str());
}

void test_instcmd_beeper(void) {
    server.setAuthenticated(0, true);
    mockHost.data.set("ups.beeper.status", "enabled");

    // Toggle beeper
    printer.clear();
    server.processCommand(printer, 0, "INSTCMD testups beeper.disable");
    TEST_ASSERT_EQUAL_STRING("OK\n", printer.getOutput().c_str());
    TEST_ASSERT_FALSE(mockHost.beeperState);

    printer.clear();
    server.processCommand(printer, 0, "INSTCMD testups beeper.enable");
    TEST_ASSERT_EQUAL_STRING("OK\n", printer.getOutput().c_str());
    TEST_ASSERT_TRUE(mockHost.beeperState);

    // Invalid command
    printer.clear();
    server.processCommand(printer, 0, "INSTCMD testups invalid.cmd");
    TEST_ASSERT_EQUAL_STRING("ERR CMD-NOT-SUPPORTED\n", printer.getOutput().c_str());
}

#ifdef PIO_UNIT_TESTING
void test_list_client_terminates(void) {
    // LIST CLIENT must be answered with a BEGIN/END pair. A bare "ERR" is what
    // breaks go.nut: it treats every "LIST " command as multi-line and only ends
    // its read loop on "END LIST ...", so a single error line blocks the client
    // until i/o timeout instead of failing fast.
    server.processCommand(printer, 0, "LIST CLIENT testups");
    TEST_ASSERT_EQUAL_STRING("BEGIN LIST CLIENT testups\nEND LIST CLIENT testups\n",
                             printer.getOutput().c_str());

    // Unknown UPS names still fall through to the shared handler, not an error.
    printer.clear();
    server.processCommand(printer, 0, "LIST CLIENT");
    TEST_ASSERT_EQUAL_STRING("BEGIN LIST CLIENT testups\nEND LIST CLIENT testups\n",
                             printer.getOutput().c_str());
}

void test_list_cmd_unaffected_by_client(void) {
    // Regression: CLIENT shares a branch with CMD/RW, so the beeper listing must
    // stay behind the CMD guard and must not leak into CLIENT.
    mockHost.data.set("ups.beeper.status", "enabled");

    printer.clear();
    server.processCommand(printer, 0, "LIST CMD testups");
    std::string cmdOut = printer.getOutput();
    TEST_ASSERT_TRUE(cmdOut.find("BEGIN LIST CMD testups\n") != std::string::npos);
    TEST_ASSERT_TRUE(cmdOut.find("CMD testups beeper.enable\n") != std::string::npos);
    TEST_ASSERT_TRUE(cmdOut.find("END LIST CMD testups\n") != std::string::npos);

    printer.clear();
    server.processCommand(printer, 0, "LIST CLIENT testups");
    TEST_ASSERT_TRUE(printer.getOutput().find("beeper") == std::string::npos);
}

void test_get_upsdesc_and_numlogins(void) {
    server.processCommand(printer, 0, "GET UPSDESC testups");
    TEST_ASSERT_EQUAL_STRING("UPSDESC testups \"ESP32-S3 UPS Bridge\"\n",
                             printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "GET NUMLOGINS testups");
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 0\n", printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "GET UPSDESC wrongups");
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "GET NUMLOGINS wrongups");
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", printer.getOutput().c_str());
}

void test_get_desc_and_type(void) {
    server.processCommand(printer, 0, "GET DESC testups ups.status");
    TEST_ASSERT_EQUAL_STRING("DESC testups ups.status \"Unavailable\"\n",
                             printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "GET TYPE testups ups.status");
    std::string typeOut = printer.getOutput();
    TEST_ASSERT_EQUAL_STRING("TYPE testups ups.status STRING:64\n", typeOut.c_str());
    // Clients read the token after "RW" as the type, so a bare RW would crash
    // them. Nothing this bridge exposes is writeable.
    TEST_ASSERT_TRUE(typeOut.find("RW") == std::string::npos);

    printer.clear();
    server.processCommand(printer, 0, "GET CMDDESC testups beeper.enable");
    TEST_ASSERT_EQUAL_STRING("CMDDESC testups beeper.enable \"Unavailable\"\n",
                             printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "GET DESC wrongups ups.status");
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "GET TYPE");
    TEST_ASSERT_EQUAL_STRING("ERR INVALID-ARGUMENT\n", printer.getOutput().c_str());
}

void test_ver_and_netver(void) {
    // go.nut sends both on every connect. It discards their errors, so these
    // were never fatal -- but answering them keeps client.Version and
    // client.ProtocolVersion meaningful instead of empty.
    server.processCommand(printer, 0, "VER");
    std::string ver = printer.getOutput();
    TEST_ASSERT_EQUAL_STRING("Network UPS Tools esp32-nut dev\n", ver.c_str());

    printer.clear();
    server.processCommand(printer, 0, "NETVER");
    TEST_ASSERT_EQUAL_STRING("1.3\n", printer.getOutput().c_str());

    // Both must answer on a single line: go.nut reads exactly one line for a
    // command that is not a LIST.
    TEST_ASSERT_EQUAL(1, (int)std::count(ver.begin(), ver.end(), '\n'));
}

void test_gonut_newups_sequence(void) {
    // go.nut's NewUPS() issues exactly these five commands, in this order, and
    // aborts on the first failure. nut_exporter reaches LIST VAR only if every
    // earlier call succeeds, so the whole sequence is the acceptance condition.
    mockHost.data.set("ups.beeper.status", "enabled");
    mockHost.data.set("battery.voltage", "13.6");

    const char* sequence[] = {
        "LIST CLIENT testups",
        "LIST CMD testups",
        "GET UPSDESC testups",
        "GET NUMLOGINS testups",
        "LIST VAR testups",
        // GetVariables() then asks these two for every variable it just listed.
        "GET DESC testups battery.voltage",
        "GET TYPE testups battery.voltage",
        // GetCommands() likewise asks for a description of every command.
        "GET CMDDESC testups beeper.enable",
    };

    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        printer.clear();
        server.processCommand(printer, 0, sequence[i]);
        std::string out = printer.getOutput();
        TEST_ASSERT_TRUE_MESSAGE(out.size() > 0, sequence[i]);
        TEST_ASSERT_TRUE_MESSAGE(out.find("ERR ") == std::string::npos, sequence[i]);
    }
}

void test_list_var_full_output(void) {
    // The computed status leads, the ups.status.* flags it is derived from stay
    // internal, and every other variable follows in insertion order.
    mockHost.statusString = "OL CHRG";
    mockHost.data.set("ups.mfr", "CPS");
    mockHost.data.set("ups.status.ac_present", "1");
    mockHost.data.set("ups.model", "Back-UPS RS 900MI");
    mockHost.data.set("ups.status.charging", "1");
    mockHost.data.set("battery.charge", "95");
    mockHost.data.set("ups.serial", "");

    server.processCommand(printer, 0, "LIST VAR testups");
    TEST_ASSERT_EQUAL_STRING("BEGIN LIST VAR testups\n"
                             "VAR testups ups.status \"OL CHRG\"\n"
                             "VAR testups ups.mfr \"CPS\"\n"
                             "VAR testups ups.model \"Back-UPS RS 900MI\"\n"
                             "VAR testups battery.charge \"95\"\n"
                             "VAR testups ups.serial \"\"\n"
                             "END LIST VAR testups\n",
                             printer.getOutput().c_str());
}

void test_quoted_values_are_escaped(void) {
    // Left bare, the quotes end the value early and the trailing backslash
    // swallows the closing quote.
    mockHost.data.set("ups.model", "Back-UPS \"XS\" 700\\");

    server.processCommand(printer, 0, "GET VAR testups ups.model");
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.model \"Back-UPS \\\"XS\\\" 700\\\\\"\n",
                             printer.getOutput().c_str());

    printer.clear();
    server.processCommand(printer, 0, "LIST VAR testups");
    TEST_ASSERT_EQUAL_STRING("BEGIN LIST VAR testups\n"
                             "VAR testups ups.status \"OL\"\n"
                             "VAR testups ups.model \"Back-UPS \\\"XS\\\" 700\\\\\"\n"
                             "END LIST VAR testups\n",
                             printer.getOutput().c_str());
}

void test_log_callback_receives_server_messages(void) {
    server.setLogCallback(captureLog);
    NUTServerConfig config;
    config.ups_name = "testups";
    server.begin(config, &mockHost, 3493);
    server.processCommand(printer, 2, "LOGOUT");

    TEST_ASSERT_EQUAL(2, (int)logged.size());
    TEST_ASSERT_EQUAL_STRING("INFO", logged[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("[NUTServer] Server listening on port 3493", logged[0].second.c_str());
    TEST_ASSERT_EQUAL_STRING("INFO", logged[1].first.c_str());
    TEST_ASSERT_EQUAL_STRING("[NUTServer] Session for slot 2 closed.", logged[1].second.c_str());
}

void test_stats_count_commands_and_auth_failures(void) {
    server.processCommand(printer, 0, "VER");
    server.processCommand(printer, 0, "   ");
    server.processCommand(printer, 0, "USERNAME admin");
    server.processCommand(printer, 0, "PASSWORD wrong");
    server.processCommand(printer, 0, "PASSWORD secret");
    server.processCommand(printer, 0, "BOGUS");

    const NUTServer::Stats& stats = server.stats();
    TEST_ASSERT_EQUAL_UINT32(5, stats.commands);
    TEST_ASSERT_EQUAL_UINT32(1, stats.authFailures);
    // Connection lifecycle counters move only in loop(), on the target.
    TEST_ASSERT_EQUAL_UINT32(0, stats.accepted);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rejected);
    TEST_ASSERT_EQUAL_UINT32(0, stats.idleTimeouts);
    TEST_ASSERT_EQUAL_UINT32(0, stats.shortWrites);
    TEST_ASSERT_EQUAL(0, server.connectedClients());
    TEST_ASSERT_EQUAL(3493, server.port());
}

// These describe the UPS rather than its readings, so they keep answering;
// go.nut's NewUPS() needs them before it lists variables.
static const char* const handshakeCommands[] = {
    "LIST UPS",
    "LIST CMD testups",
    "GET UPSDESC testups",
    "GET NUMLOGINS testups",
};

static void assertVarReadsRefusedWith(const char* expected) {
    const char* reads[] = {
        "LIST VAR testups",
        "GET VAR testups battery.charge",
        "GET VAR testups ups.status",
    };
    for (const char* command : reads) {
        printer.clear();
        server.processCommand(printer, 0, command);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, printer.getOutput().c_str(), command);
    }
    for (const char* command : handshakeCommands) {
        printer.clear();
        server.processCommand(printer, 0, command);
        std::string out = printer.getOutput();
        TEST_ASSERT_TRUE_MESSAGE(out.size() > 0, command);
        TEST_ASSERT_TRUE_MESSAGE(out.find("ERR ") == std::string::npos, command);
    }
}

void test_var_reads_refused_when_driver_not_connected(void) {
    mockHost.data.set("battery.charge", "95");
    mockHost.connected = false;
    assertVarReadsRefusedWith("ERR DRIVER-NOT-CONNECTED\n");

    // As in upsd, the UPS name is checked first.
    printer.clear();
    server.processCommand(printer, 0, "LIST VAR wrongups");
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", printer.getOutput().c_str());
}

void test_var_reads_refused_without_ups(void) {
    NUTServerConfig config;
    config.ups_name = "testups";
    server.begin(config, nullptr, 3493);
    assertVarReadsRefusedWith("ERR DRIVER-NOT-CONNECTED\n");
}

void test_var_reads_refused_when_data_stale(void) {
    mockHost.data.set("battery.charge", "95");
    mockHost.fresh = false;
    assertVarReadsRefusedWith("ERR DATA-STALE\n");
    TEST_ASSERT_EQUAL_UINT32(90000, mockHost.lastMaxAge);
}

static std::string reply(int slot, const char* command) {
    printer.clear();
    server.processCommand(printer, slot, command);
    return printer.getOutput();
}

void test_protver_answers_as_netver(void) {
    TEST_ASSERT_EQUAL_STRING("1.3\n", reply(0, "PROTVER").c_str());
}

void test_starttls_not_supported(void) {
    TEST_ASSERT_EQUAL_STRING("ERR FEATURE-NOT-SUPPORTED\n", reply(0, "STARTTLS").c_str());
}

void test_numlogins_counts_logged_in_sessions(void) {
    // A primary's upsmon waits for this to fall to 1, itself, before shutting
    // down, so every session that leaves must drop out of it.
    server.setAuthenticated(0, true);
    server.setAuthenticated(1, true);

    TEST_ASSERT_EQUAL_STRING("OK\n", reply(0, "LOGIN testups").c_str());
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 1\n", reply(3, "GET NUMLOGINS testups").c_str());
    TEST_ASSERT_EQUAL_STRING("OK\n", reply(0, "LOGIN testups").c_str());
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 1\n", reply(3, "GET NUMLOGINS testups").c_str());
    TEST_ASSERT_EQUAL_STRING("OK\n", reply(1, "LOGIN testups").c_str());
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 2\n", reply(3, "GET NUMLOGINS testups").c_str());

    // Refused logins do not count.
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(2, "LOGIN testups").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", reply(2, "LOGIN wrongups").c_str());
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 2\n", reply(3, "GET NUMLOGINS testups").c_str());

    TEST_ASSERT_EQUAL_STRING("OK Goodbye\n", reply(0, "LOGOUT").c_str());
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 1\n", reply(3, "GET NUMLOGINS testups").c_str());
    TEST_ASSERT_EQUAL_STRING("OK Goodbye\n", reply(1, "LOGOUT").c_str());
    TEST_ASSERT_EQUAL_STRING("NUMLOGINS testups 0\n", reply(3, "GET NUMLOGINS testups").c_str());
}

void test_primary_and_master_need_authentication(void) {
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "PRIMARY testups").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "MASTER testups").c_str());

    TEST_ASSERT_EQUAL_STRING("OK\n", reply(0, "USERNAME admin").c_str());
    TEST_ASSERT_EQUAL_STRING("OK\n", reply(0, "PASSWORD secret").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR INVALID-ARGUMENT\n", reply(0, "PRIMARY").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", reply(0, "PRIMARY wrongups").c_str());
    TEST_ASSERT_EQUAL_STRING("OK PRIMARY-GRANTED\n", reply(0, "PRIMARY testups").c_str());
    TEST_ASSERT_EQUAL_STRING("OK MASTER-GRANTED\n", reply(0, "master TESTUPS").c_str());
}

void test_primary_refused_without_credentials_configured(void) {
    // Anyone who reached the port could otherwise raise FSD and shut down
    // every secondary.
    NUTServerConfig open;
    open.ups_name = "testups";
    server.begin(open, &mockHost, 3493);

    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "PRIMARY testups").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "FSD testups").c_str());

    // Still refused should a session on an open device ever count as
    // authenticated.
    server.setAuthenticated(0, true);
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "MASTER testups").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "FSD testups").c_str());
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.status \"OL\"\n",
                             reply(1, "GET VAR testups ups.status").c_str());
}

void test_fsd_needs_primary(void) {
    server.setAuthenticated(0, true);
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "FSD testups").c_str());

    TEST_ASSERT_EQUAL_STRING("OK PRIMARY-GRANTED\n", reply(0, "PRIMARY testups").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR INVALID-ARGUMENT\n", reply(0, "FSD").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", reply(0, "FSD wrongups").c_str());
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.status \"OL\"\n",
                             reply(1, "GET VAR testups ups.status").c_str());

    // Primary goes with the session.
    reply(0, "LOGOUT");
    server.setAuthenticated(0, true);
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "FSD testups").c_str());
}

void test_fsd_stays_in_ups_status(void) {
    // Secondaries shut down on seeing FSD, so it must outlive the primary's
    // session and any later change in the UPS's own status.
    server.setLogCallback(captureLog);
    server.setAuthenticated(0, true);
    reply(0, "PRIMARY testups");
    TEST_ASSERT_EQUAL_STRING("OK FSD-SET\n", reply(0, "FSD testups").c_str());

    TEST_ASSERT_EQUAL(1, (int)logged.size());
    TEST_ASSERT_EQUAL_STRING("WARN", logged[0].first.c_str());
    TEST_ASSERT_TRUE(logged[0].second.find("FSD set on testups") != std::string::npos);

    TEST_ASSERT_EQUAL_STRING("VAR testups ups.status \"OL FSD\"\n",
                             reply(1, "GET VAR testups ups.status").c_str());

    reply(0, "LOGOUT");
    mockHost.statusString = "OB LB";
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.status \"OB LB FSD\"\n",
                             reply(1, "GET VAR testups ups.status").c_str());
    TEST_ASSERT_EQUAL_STRING("BEGIN LIST VAR testups\n"
                             "VAR testups ups.status \"OB LB FSD\"\n"
                             "END LIST VAR testups\n",
                             reply(1, "LIST VAR testups").c_str());

    // Not doubled when the UPS itself reports FSD.
    mockHost.statusString = "OB LB FSD";
    TEST_ASSERT_EQUAL_STRING("VAR testups ups.status \"OB LB FSD\"\n",
                             reply(1, "GET VAR testups ups.status").c_str());
}

void test_set_var_is_refused(void) {
    mockHost.data.set("battery.charge", "95");
    TEST_ASSERT_EQUAL_STRING("ERR ACCESS-DENIED\n", reply(0, "SET VAR testups battery.charge 50").c_str());

    server.setAuthenticated(0, true);
    TEST_ASSERT_EQUAL_STRING("ERR READONLY\n", reply(0, "SET VAR testups battery.charge 50").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR READONLY\n", reply(0, "set var testups UPS.STATUS \"OL\"").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR VAR-NOT-SUPPORTED\n", reply(0, "SET VAR testups input.voltage 230").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN-UPS\n", reply(0, "SET VAR wrongups battery.charge 50").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR INVALID-ARGUMENT\n", reply(0, "SET VAR testups battery.charge").c_str());
    TEST_ASSERT_EQUAL_STRING("ERR INVALID-ARGUMENT\n", reply(0, "SET TRACKING ON").c_str());
}

void test_driver_info_listed_like_other_variables(void) {
    CyberPowerDriver driver;
    driver.publishDriverInfo(mockHost.data);

    TEST_ASSERT_EQUAL_STRING("BEGIN LIST VAR testups\n"
                             "VAR testups ups.status \"OL\"\n"
                             "VAR testups driver.name \"esp32-nut\"\n"
                             "VAR testups driver.version \"dev\"\n"
                             "VAR testups driver.version.data \"CyberPowerDriver\"\n"
                             "END LIST VAR testups\n",
                             reply(0, "LIST VAR testups").c_str());
    TEST_ASSERT_EQUAL_STRING("VAR testups driver.version.data \"CyberPowerDriver\"\n",
                             reply(0, "GET VAR testups driver.version.data").c_str());
}

void test_replies_written_once_outside_usb_lock(void) {
    // Every write can block the loop for 10 s on a peer that stops reading, and
    // one made under the USB data lock stalls the USB task along with it.
    mockHost.data.set("ups.beeper.status", "enabled");
    mockHost.data.set("battery.charge", "95");
    server.setAuthenticated(0, true);

    const char* commands[] = {
        "LIST UPS",
        "LIST VAR testups",
        "LIST CMD testups",
        "LIST ENUM testups battery.charge",
        "GET VAR testups battery.charge",
        "GET VAR testups ups.status",
        "GET VAR testups input.voltage",
        "SET VAR testups battery.charge 50",
    };

    for (const char* command : commands) {
        SocketPrinter socket;
        server.processCommand(socket, 0, command);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, socket.writes, command);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, socket.writesUnderLock, command);
    }
}

#ifndef ARDUINO
int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_split_tokens);
    RUN_TEST(test_auth_flow);
    RUN_TEST(test_list_ups);
    RUN_TEST(test_get_var_compliance);
    RUN_TEST(test_instcmd_beeper);
    RUN_TEST(test_list_client_terminates);
    RUN_TEST(test_list_cmd_unaffected_by_client);
    RUN_TEST(test_get_upsdesc_and_numlogins);
    RUN_TEST(test_get_desc_and_type);
    RUN_TEST(test_ver_and_netver);
    RUN_TEST(test_gonut_newups_sequence);
    RUN_TEST(test_list_var_full_output);
    RUN_TEST(test_quoted_values_are_escaped);
    RUN_TEST(test_log_callback_receives_server_messages);
    RUN_TEST(test_stats_count_commands_and_auth_failures);
    RUN_TEST(test_var_reads_refused_when_driver_not_connected);
    RUN_TEST(test_var_reads_refused_without_ups);
    RUN_TEST(test_var_reads_refused_when_data_stale);
    RUN_TEST(test_protver_answers_as_netver);
    RUN_TEST(test_starttls_not_supported);
    RUN_TEST(test_numlogins_counts_logged_in_sessions);
    RUN_TEST(test_primary_and_master_need_authentication);
    RUN_TEST(test_primary_refused_without_credentials_configured);
    RUN_TEST(test_fsd_needs_primary);
    RUN_TEST(test_fsd_stays_in_ups_status);
    RUN_TEST(test_set_var_is_refused);
    RUN_TEST(test_driver_info_listed_like_other_variables);
    RUN_TEST(test_replies_written_once_outside_usb_lock);
    return UNITY_END();
}
#else
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_split_tokens);
    RUN_TEST(test_auth_flow);
    RUN_TEST(test_list_ups);
    RUN_TEST(test_get_var_compliance);
    RUN_TEST(test_instcmd_beeper);
    RUN_TEST(test_list_client_terminates);
    RUN_TEST(test_list_cmd_unaffected_by_client);
    RUN_TEST(test_get_upsdesc_and_numlogins);
    RUN_TEST(test_get_desc_and_type);
    RUN_TEST(test_ver_and_netver);
    RUN_TEST(test_gonut_newups_sequence);
    RUN_TEST(test_list_var_full_output);
    RUN_TEST(test_quoted_values_are_escaped);
    RUN_TEST(test_log_callback_receives_server_messages);
    RUN_TEST(test_stats_count_commands_and_auth_failures);
    RUN_TEST(test_var_reads_refused_when_driver_not_connected);
    RUN_TEST(test_var_reads_refused_without_ups);
    RUN_TEST(test_var_reads_refused_when_data_stale);
    RUN_TEST(test_protver_answers_as_netver);
    RUN_TEST(test_starttls_not_supported);
    RUN_TEST(test_numlogins_counts_logged_in_sessions);
    RUN_TEST(test_primary_and_master_need_authentication);
    RUN_TEST(test_primary_refused_without_credentials_configured);
    RUN_TEST(test_fsd_needs_primary);
    RUN_TEST(test_fsd_stays_in_ups_status);
    RUN_TEST(test_set_var_is_refused);
    RUN_TEST(test_driver_info_listed_like_other_variables);
    RUN_TEST(test_replies_written_once_outside_usb_lock);
    UNITY_END();
}
void loop() {}
#endif
#endif

