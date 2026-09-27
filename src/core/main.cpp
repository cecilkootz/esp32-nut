#include "core/main.h"
#include "USBHostUPS.h"
#include "network/web_config_server.h"
#include "core/app_logger.h"
#include "core/memory_stats.h"
#include <Preferences.h>
#include <esp_task_wdt.h>

USBHostUPS usb_ups;
ConfigManager config_mgr;
AppNetworkManager network_mgr;
NUTServer nut_server;
DiagnosticLED diagnostic_led;
WebConfigServer web_server(config_mgr);

Preferences boot_prefs;
bool is_ap_mode = false;
bool clear_ap_flag_pending = false;
uint32_t boot_time_ms = 0;

// Calcola lo stato diagnostico del sistema a partire dallo stato Wi-Fi e UPS
LedState computeSystemState(bool wifiConnected, bool upsConnected) {
    if (is_ap_mode) {
        return LedState::AP_MODE; // AP mode = AP_MODE
    }
    if (!wifiConnected) {
        return LedState::CONNECTING;
    }
    if (!upsConnected) {
        return LedState::ERROR;
    }
    return LedState::OPERATIONAL;
}

#ifndef UNIT_TEST
void setup() {
    // Inizializzazione della porta seriale per il debug diagnostico
    Serial.begin(MONITOR_BAUD_RATE);
    delay(1000); // Piccolo delay per stabilizzare la connessione seriale
    AppLogger::log("INFO", "\n--- ESP32 NUT Server Initialized ---");

    // Inizializzazione del LED diagnostico
    diagnostic_led.begin(LED_BUILTIN_PIN);

    // Gestione NVS flag per AP manuale
    boot_prefs.begin("boot_state", false);
    bool manual_ap_triggered = boot_prefs.getBool("manual_ap", false);

    if (manual_ap_triggered) {
        boot_prefs.putBool("manual_ap", false);
        AppLogger::log("INFO", "[MAIN] Manual AP trigger detected!");
    } else {
        boot_prefs.putBool("manual_ap", true);
        clear_ap_flag_pending = true;
        boot_time_ms = millis();
    }

    // Inizializzazione di ConfigManager
    bool config_ok = config_mgr.begin() && config_mgr.isValid();
    
    if (!config_ok || manual_ap_triggered) {
        is_ap_mode = true;
        AppLogger::log("WARN", "[MAIN] Starting in AP mode...");
        network_mgr.beginAP("NUT_ESP32_Config", "12345678");
        web_server.setUPS(&usb_ups);
        web_server.begin(true);
    } else {
        is_ap_mode = false;
        AppLogger::log("INFO", "[MAIN] Configuration loaded successfully.");
        // Stampa parametri per verifica
        WifiConfig wifi = config_mgr.getWifiConfig();
        NutConfig nut = config_mgr.getNutConfig();
        AppLogger::log("INFO", "[MAIN] Wi-Fi SSID: %s\n", wifi.ssid.c_str());
        AppLogger::log("INFO", "[MAIN] NUT UPS Name: %s\n", nut.ups_name.c_str());
        
        // Inizializzazione di AppNetworkManager
        network_mgr.begin(wifi.ssid, wifi.password);
        web_server.setUPS(&usb_ups);
        web_server.begin(false);
    }

    // Inizializzazione della libreria USBHostUPS (sempre attiva)
    usb_ups.setLogCallback([](const char* level, const char* msg) {
        AppLogger::log(level, "%s", msg);
    });

    // Delay USB host initialization on cold boot to allow the UPS USB interface to stabilize
    // Many UPS devices (like APC) take a few seconds to properly handle USB requests after power on.
    uint32_t wait_until = 3000;
    while (millis() < wait_until) {
        delay(10);
    }

    if (!usb_ups.begin()) {
        AppLogger::log("ERROR", "[MAIN] ERROR: USBHostUPS initialization failed!");
    } else {
        AppLogger::log("INFO", "[MAIN] USBHostUPS initialized correctly.");
    }

    // Inizializzazione NUTServer (solo se la configurazione è valida)
    if (config_ok) {
        NutConfig nut_config = config_mgr.getNutConfig();
        NUTServerConfig nut_server_config = {nut_config.username, nut_config.password, nut_config.ups_name};
        if (!nut_server.begin(nut_server_config, &usb_ups)) {
            AppLogger::log("ERROR", "[MAIN] ERROR: NUTServer initialization failed!");
        } else {
            AppLogger::log("INFO", "[MAIN] NUTServer started correctly on port 3493.");
        }
    }

    // Everything runs in loop(), so a stalled iteration leaves the device
    // unreachable until it is power-cycled. 30 s clears the longest legitimate
    // single iteration; the core feeds the watchdog before each one.
    const esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 30000,
        .idle_core_mask = 1 << 0, // as in sdkconfig: only CPU0's idle task
        .trigger_panic = true,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err != ESP_OK) {
        // The 5 s boot default would trip on legitimate waits, so stay unwatched.
        AppLogger::log("ERROR", "[MAIN] Task watchdog reconfigure failed (%s), loop not watched", esp_err_to_name(wdt_err));
    } else {
        enableLoopWDT();
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            AppLogger::log("INFO", "[MAIN] Loop watchdog enabled (30 s).");
        } else {
            AppLogger::log("ERROR", "[MAIN] Failed to subscribe the loop task to the task watchdog");
        }
    }
}

void loop() {
    uint32_t now = millis();

    // Check timer per azzeramento flag manual_ap
    if (clear_ap_flag_pending && (now - boot_time_ms > 3000)) {
        boot_prefs.putBool("manual_ap", false);
        clear_ap_flag_pending = false;
        AppLogger::log("INFO", "[MAIN] Manual AP trigger window closed.");
    }

    web_server.loop();
    network_mgr.loop();
    usb_ups.loop();

    // Se la configurazione non è valida, rimaniamo in modalità di attesa sicura
    if (!config_mgr.isValid()) {
        static uint32_t last_safe_print = 0;
        if (now - last_safe_print >= 5000) {
            last_safe_print = now;
            AppLogger::log("WARN", "[MAIN] WARNING: System in safe waiting mode. Configuration missing or invalid!");
        }
        
        diagnostic_led.setState(computeSystemState(network_mgr.isConnected(), usb_ups.isConnected()));
        diagnostic_led.update();
        delay(10);
        return;
    }

    nut_server.loop();

    static uint32_t last_print = 0;
    if (now - last_print >= 5000) {
        last_print = now;
        MemoryStats mem = readMemoryStats();
        AppLogger::log("INFO", "[DIAG] UPS Info: Battery = %d%% | Status = %s | Voltage = %.1f V"
                               " | Heap free=%lu min=%lu largest=%lu | Stack min free loop=%lu hid=%ld",
                      (int)usb_ups.getUPSData()->getFloat("battery.charge"),
                      usb_ups.getUPSStatusString().c_str(),
                      usb_ups.getUPSData()->getFloat("output.voltage"),
                      (unsigned long)mem.free_heap,
                      (unsigned long)mem.min_free_heap,
                      (unsigned long)mem.largest_free_block,
                      (unsigned long)mem.loop_stack_min_free,
                      (long)mem.hid_stack_min_free);
    }

    // Aggiornamento stato LED diagnostico
    diagnostic_led.setState(computeSystemState(network_mgr.isConnected(), usb_ups.isConnected()));
    diagnostic_led.update();

    delay(10);
}
#endif

