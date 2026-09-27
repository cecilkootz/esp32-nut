#include "WebApiJson.h"
#include "network/web_config_server.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include "core/app_logger.h"
#include "core/system_status.h"
#include "network/mqtt_bridge.h"
#include "network/web_assets.h"
#include <Update.h>

WebConfigServer::WebConfigServer(ConfigManager& config_mgr) 
    : server(80), config_mgr(config_mgr), is_ap_mode(false) {}

void WebConfigServer::begin(bool isAPMode) {
    this->is_ap_mode = isAPMode;
    
    if (is_ap_mode) {
        // Start DNS Server for Captive Portal
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(53, "*", WiFi.softAPIP());
    }

    static bool _handlers_registered = false;
    if (_handlers_registered) return;
    _handlers_registered = true;

    // Serve explicitly the index.html on root
    server.on("/", HTTP_GET, [this]() {
        server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        server.sendHeader("Pragma", "no-cache");
        server.sendHeader("Expires", "-1");
        server.sendHeader("Content-Encoding", "gzip");
        server.send_P(200, "text/html", (const char*)web_asset_index_inlined_html, web_asset_index_inlined_html_len);
    });

    server.on("/index.html", HTTP_GET, [this]() {
        server.sendHeader("Location", "/", true);
        server.send(301, "text/plain", "");
    });


    // REST endpoints
    server.on("/api/wifi/connect", HTTP_POST, [this]() { handleConnect(); });
    server.on("/api/nut/config", HTTP_POST, [this]() { handleNutConfig(); });
    server.on("/api/mqtt/config", HTTP_POST, [this]() { handleMqttConfig(); });
    server.on("/api/logs", HTTP_GET, [this]() { handleLogs(); });
    server.on("/api/config", HTTP_GET, [this]() { handleGetConfig(); });
    server.on("/api/ups-vars", HTTP_GET, [this]() { handleUpsVars(); });
    server.on("/api/system-status", HTTP_GET, [this]() { handleSystemStatus(); });
    server.on("/api/beeper", HTTP_POST, [this]() { handleBeeper(); });
    server.on("/api/usb/dump", HTTP_GET, [this]() {
        if (!usb_ups) {
            server.send(503, "application/json", "{\"error\": \"UPS non inizializzato\"}");
            return;
        }
        server.sendHeader("Content-Disposition", "attachment; filename=\"usb_diagnostics.json\"");
        server.send(200, "application/json", usb_ups->dumpUSBDiagnostics());
    });

    // OTA endpoints
    server.on("/update", HTTP_GET, [this]() { handleOTAPage(); });
    server.on("/update", HTTP_POST, 
        [this]() { 
            server.sendHeader("Connection", "close");
            server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            if (!Update.hasError()) {
                should_restart = true;
                restart_request_time = millis();
            }
        },
        [this]() { handleOTAUpload(); }
    );

    // Catch-all for Captive Portal (redirect to captive page if not found)
    server.onNotFound([this]() {
        if (is_ap_mode && server.hostHeader() != WiFi.softAPIP().toString()) {
            server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
            server.send(302, "text/plain", "");
        } else {
            server.send(404, "text/plain", "Not found");
        }
    });

    server.begin();
    AppLogger::log("INFO", "[WEB] Server started on port 80");
}

void WebConfigServer::loop() {
    if (is_ap_mode) {
        dnsServer.processNextRequest();
    }
    server.handleClient();

    if (should_restart && (millis() - restart_request_time >= 1000)) {
        if (mqtt_bridge) {
            mqtt_bridge->end();
        }
        if (usb_ups) {
            usb_ups->end();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP.restart();
    }
}

void WebConfigServer::setUPS(USBHostUPS* ups) {
    usb_ups = ups;
}

void WebConfigServer::setNetwork(AppNetworkManager* network) {
    network_mgr = network;
}

void WebConfigServer::setNUT(NUTServer* nut) {
    nut_server = nut;
}

void WebConfigServer::setMqtt(MqttBridge* mqtt) {
    mqtt_bridge = mqtt;
}

void WebConfigServer::handleConnect() {
    if (server.hasArg("plain") == false) {
        server.send(400, "application/json", "{\"error\": \"Body not received\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));

    if (error) {
        server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
        return;
    }

    // as<String>() turns a missing key into "null"
    String ssid = doc["ssid"] | "";
    String pwd = doc["password"] | "";
    if (ssid.isEmpty()) {
        server.send(400, "application/json", "{\"error\": \"SSID required\"}");
        return;
    }

    WifiConfig wc;
    wc.ssid = ssid;
    wc.password = pwd;

    config_mgr.setWifiConfig(wc);
    if (config_mgr.save()) {
        server.send(200, "application/json", "{\"success\": true}");
        
        // Asynchronous restart
        should_restart = true;
        restart_request_time = millis();
    } else {
        server.send(500, "application/json", "{\"error\": \"Failed to save config\"}");
    }
}

void WebConfigServer::handleLogs() {
    server.send(200, "application/json", AppLogger::getLogsJSON());
}

void WebConfigServer::handleNutConfig() {
    if (server.hasArg("plain") == false) {
        server.send(400, "application/json", "{\"error\": \"Body not received\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));

    if (error) {
        server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
        return;
    }

    NutConfig nc = config_mgr.getNutConfig();
    if (doc["username"].is<String>()) {
        nc.username = doc["username"].as<String>();
    }
    if (doc["password"].is<String>()) {
        nc.password = doc["password"].as<String>();
    }
    if (doc["ups_name"].is<String>()) {
        nc.ups_name = doc["ups_name"].as<String>();
    }

    config_mgr.setNutConfig(nc);
    if (config_mgr.save()) {
        server.send(200, "application/json", "{\"success\": true}");
        AppLogger::log("INFO", "[WEB] NUT configuration updated");
        // Unconfigured, the NUT server isn't running: a restart would only drop the setup
        // hotspot. The Wi-Fi save applies these settings.
        if (config_mgr.isValid()) {
            should_restart = true;
            restart_request_time = millis();
        }
    } else {
        server.send(500, "application/json", "{\"error\": \"Failed to save config\"}");
    }
}

void WebConfigServer::handleMqttConfig() {
    if (server.hasArg("plain") == false) {
        server.send(400, "application/json", "{\"error\": \"Body not received\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));

    if (error) {
        server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
        return;
    }

    // Saving would also store the empty Wi-Fi settings of an unconfigured board
    if (!config_mgr.isValid()) {
        server.send(409, "application/json", "{\"error\": \"Configure Wi-Fi and NUT first\"}");
        return;
    }

    MqttConfig mc = config_mgr.getMqttConfig();
    if (doc["host"].is<String>()) {
        mc.host = doc["host"].as<String>();
        mc.host.trim();
    }
    if (doc["port"].is<int>()) {
        int port = doc["port"].as<int>();
        if (port < 1 || port > 65535) {
            server.send(400, "application/json", "{\"error\": \"Invalid port\"}");
            return;
        }
        mc.port = port;
    }
    if (doc["username"].is<String>()) {
        mc.username = doc["username"].as<String>();
    }
    // Left out, the saved password stays: the web UI never shows it
    if (doc["password"].is<String>()) {
        mc.password = doc["password"].as<String>();
    }

    config_mgr.setMqttConfig(mc);
    if (config_mgr.save()) {
        server.send(200, "application/json", "{\"success\": true}");
        AppLogger::log("INFO", "[WEB] MQTT configuration updated");
        if (mc.host.length() == 0 && mqtt_bridge) {
            mqtt_bridge->forget();
        }
        should_restart = true;
        restart_request_time = millis();
    } else {
        server.send(500, "application/json", "{\"error\": \"Failed to save config\"}");
    }
}

void WebConfigServer::handleGetConfig() {
    JsonDocument doc;
    
    JsonObject wifiObj = doc["wifi"].to<JsonObject>();
    wifiObj["ssid"] = config_mgr.getWifiConfig().ssid;
    wifiObj["mode"] = is_ap_mode ? "AP" : "STA";
    
    JsonObject nutObj = doc["nut"].to<JsonObject>();
    nutObj["username"] = config_mgr.getNutConfig().username;
    nutObj["ups_name"] = config_mgr.getNutConfig().ups_name;

    MqttConfig mqtt = config_mgr.getMqttConfig();
    JsonObject mqttObj = doc["mqtt"].to<JsonObject>();
    mqttObj["host"] = mqtt.host;
    mqttObj["port"] = mqtt.port;
    mqttObj["username"] = mqtt.username;
    mqttObj["password_set"] = mqtt.password.length() > 0;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WebConfigServer::handleUpsVars() {
    if (!usb_ups) {
        server.send(503, "application/json", "{\"error\": \"UPS non inizializzato\"}");
        return;
    }
    String response = WebApiJson::generateUpsVars(usb_ups);
    server.send(200, "application/json", response);
}

void WebConfigServer::handleSystemStatus() {
    SystemStatusSources sources;
    sources.ap_mode = is_ap_mode;
    sources.network = network_mgr;
    sources.ups = usb_ups;
    sources.nut = nut_server;
    sources.mqtt = mqtt_bridge;

    JsonDocument doc;
    fillSystemStatus(doc, sources);
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WebConfigServer::handleBeeper() {
    if (server.hasArg("plain") == false) {
        server.send(400, "application/json", "{\"error\": \"Body not received\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));

    if (error) {
        server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
        return;
    }

    if (doc["enable"].is<bool>()) {
        bool enable = doc["enable"].as<bool>();
        if (usb_ups) {
            usb_ups->setBeeper(enable);
            server.send(200, "application/json", "{\"success\": true}");
            return;
        }
    }
    
    server.send(500, "application/json", "{\"error\": \"Failed to set beeper\"}");
}

void WebConfigServer::handleOTAPage() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)web_asset_update_inlined_html, web_asset_update_inlined_html_len);
}

void WebConfigServer::handleOTAUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        AppLogger::log("INFO", String("[OTA] Upload started: ") + upload.filename);
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace, U_FLASH)) { //start with max available size
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        // The whole upload is parsed within a single loop() iteration.
        feedLoopWDT();
        // Validazione magic byte E9 sul primo chunk
        if (upload.totalSize == 0 && upload.currentSize > 0) {
            if (upload.buf[0] != 0xE9) {
                AppLogger::log("ERROR", "[OTA] Invalid magic byte");
                Update.abort();
                return;
            }
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { //true to set the size to the current progress
            AppLogger::log("INFO", String("[OTA] Success: ") + String(upload.totalSize) + " bytes");
        } else {
            Update.printError(Serial);
            AppLogger::log("ERROR", "[OTA] Error at the end");
        }
    }
}

