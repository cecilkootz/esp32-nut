#include "BeeperLogic.h"
#include "USBHostUPS.h"
#include "GenericDriver.h"
#include "APCDriver.h"
#include "PowercomDriver.h"
#include "EatonDriver.h"
#include "CyberPowerDriver.h"
#include "OpenUPSDriver.h"
#include "GoldenMateDriver.h"
#include <ArduinoJson.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// Several seconds of a once-a-second interrupt stream, in case the loop task is
// held up by a slow control transfer.
static const UBaseType_t REPORT_QUEUE_DEPTH = 8;
// Connects and disconnects share one queue so loop() sees them in order.
static const UBaseType_t EVENT_QUEUE_DEPTH = 8;

USBHostUPS::USBHostUPS() :
    _hid_dev_handle(NULL), _dev_handle(NULL),
    _vid(0), _pid(0),
    _initialized(false), _is_ready_to_poll(false),
    _is_fetching(false), _control_pending(false),
    _driver(nullptr), _log_cb(nullptr), _quirks(0), _usb_task_handle(NULL), _usb_task_run(false)
{
}

USBHostUPS::~USBHostUPS() {
    end();
    if (_report_queue) vQueueDelete(_report_queue);
    if (_event_queue) vQueueDelete(_event_queue);
}

bool USBHostUPS::begin() {
    if (_initialized) return true;

    if (!_report_queue) _report_queue = xQueueCreate(REPORT_QUEUE_DEPTH, sizeof(InputReport));
    if (!_event_queue) _event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(HidEvent));
    if (!_report_queue || !_event_queue) {
        if (_log_cb) _log_cb("ERROR", "USB event queue allocation failed");
        return false;
    }

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (_log_cb) _log_cb("ERROR", "usb_host_install failed");
        return false;
    }

    if (!_usb_task_handle) {
        _usb_task_run = true;
        xTaskCreatePinnedToCore(
            USBHostUPS::usb_host_lib_task,
            "usb_host_events",
            4096,
            this,
            2,
            &_usb_task_handle,
            tskNO_AFFINITY
        );
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = USBHostUPS::hid_host_driver_event_cb,
        .callback_arg = this
    };

    err = hid_host_install(&hid_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (_log_cb) _log_cb("ERROR", "hid_host_install failed");
        return false;
    }

    _initialized = true;
    return true;
}

void USBHostUPS::end() {
    if (!_initialized) return;

    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        std::lock_guard<std::recursive_mutex> io(_io_mutex);
        if (_hid_dev_handle) {
            hid_host_device_close(_hid_dev_handle);
            reset_device_state();
        }
    }

    hid_host_uninstall();
    
    if (_usb_task_handle) {
        _usb_task_run = false;
        // The task will exit on next wakeup, or we can just let it clean up on shutdown if possible.
        // Espressif's usb_host_lib requires all clients to be deregistered before it can be uninstalled.
        // Actually, we don't strictly need to kill it unless we want to totally uninstall usb_host.
        _usb_task_handle = NULL;
    }
    _initialized = false;
}

void USBHostUPS::hid_host_driver_event_cb(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg) {
    USBHostUPS* ups = static_cast<USBHostUPS*>(arg);
    ups->handle_driver_event(hid_device_handle, event);
}

void USBHostUPS::handle_driver_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event) {
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        const hid_host_device_config_t dev_config = {
            .callback = USBHostUPS::hid_host_interface_event_cb,
            .callback_arg = this
        };

        // Opening takes only the library's own lock. Claiming needs control
        // transfers, which complete on this task, so loop() does that.
        esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
        post_event(err == ESP_OK ? HidEvent::CONNECTED : HidEvent::OPEN_FAILED, hid_device_handle);
    }
}

void USBHostUPS::hid_host_interface_event_cb(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg) {
    USBHostUPS* ups = static_cast<USBHostUPS*>(arg);
    ups->handle_interface_event(hid_device_handle, event);
}

// Runs on the HID task, or on the loop task when the app closes an interface
// itself. Never takes _mutex: loop() holds it across control transfers, and
// this task is the one that completes them.
void USBHostUPS::handle_interface_event(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event) {
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        InputReport report;
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(hid_device_handle, report.data, sizeof(report.data), &length) != ESP_OK) return;
        report.handle = hid_device_handle;
        report.len = (uint8_t)length;
        if (xQueueSend(_report_queue, &report, 0) != pdTRUE) {
            // Drop the oldest instead: the newest report carries the current state.
            InputReport oldest;
            xQueueReceive(_report_queue, &oldest, 0);
            xQueueSend(_report_queue, &report, 0);
            _interrupt_reports_dropped++;
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        {
            // The library frees this interface, then its device, once we return,
            // so let a loop-task call still inside them finish. IDF delivers the
            // departed device's pending control transfer before reporting it
            // gone, so what is left of that call does not need this task.
            std::lock_guard<std::recursive_mutex> io(_io_mutex);
            if (hid_device_handle == _hid_dev_handle) _is_ready_to_poll = false;
            hid_host_device_close(hid_device_handle);
        }
        post_event(HidEvent::DISCONNECTED, hid_device_handle);
    }
}

void USBHostUPS::loop() {
    if (!_initialized) return;

    std::lock_guard<std::recursive_mutex> lock(_mutex);
    drain_reports();
    drain_events();

    if (_is_ready_to_poll && _driver) {
        _driver->loop(this, _ups_data, millis());
    }
}

void USBHostUPS::post_event(HidEvent::Kind kind, hid_host_device_handle_t handle) {
    const HidEvent event = { kind, handle };
    if (xQueueSend(_event_queue, &event, 0) != pdTRUE) _events_dropped++;
}

void USBHostUPS::drain_reports() {
    InputReport report;
    while (xQueueReceive(_report_queue, &report, 0) == pdTRUE) {
        if (report.handle == _hid_dev_handle && _driver) process_input_report(report);
    }
}

void USBHostUPS::process_input_report(const InputReport& report) {
    const uint8_t* data = report.data;
    size_t length = report.len;
    uint8_t r_id = (length > 0) ? data[0] : 0;
    // This fires on every interrupt report, roughly once a second, and the
    // log is a 50-entry ring: logging each one buries everything else
    // within a minute. Report the stream's shape only when it changes.
    if (r_id != _last_logged_interrupt_id || length != _last_logged_interrupt_len) {
        _last_logged_interrupt_id = r_id;
        _last_logged_interrupt_len = length;
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "INPUT_REPORT: id=%d, len=%d", r_id, length);
        if (_log_cb) _log_cb("INFO", dbg);
    }

    if (length > 0) {
        std::vector<uint8_t> payload(data, data + length);
        uint16_t key = (1 << 8) | r_id; // type 1 = INPUT
        _cached_reports[key] = {r_id, 1, payload};
        _interrupt_report_seen[r_id] = millis();
    }

    _driver->decodeReport(this, r_id, 1, data, length, _ups_data);
}

void USBHostUPS::drain_events() {
    HidEvent event;
    while (xQueueReceive(_event_queue, &event, 0) == pdTRUE) {
        if (event.kind == HidEvent::DISCONNECTED) {
            if (_log_cb) _log_cb("INFO", "HID Device Disconnected");
            // A replace in claim_interface() has already reset the old device,
            // and by now _hid_dev_handle is the new one.
            if (event.handle == _hid_dev_handle) reset_device_state();
            continue;
        }

        if (_log_cb) _log_cb("INFO", "HID Device Connected event");
        if (event.kind == HidEvent::OPEN_FAILED) {
            if (_log_cb) _log_cb("ERROR", "hid_host_device_open failed");
            continue;
        }
        claim_interface(event.handle);
    }

    uint32_t dropped = _events_dropped;
    if (dropped != _events_dropped_logged) {
        _events_dropped_logged = dropped;
        if (_log_cb) _log_cb("ERROR", "HID event queue overflow: connect/disconnect events lost");
    }
}

void USBHostUPS::claim_interface(hid_host_device_handle_t handle) {
    // Held throughout: desc is library memory that a disconnect frees.
    std::lock_guard<std::recursive_mutex> io(_io_mutex);

    size_t desc_len = 0;
    uint8_t *desc = hid_host_get_report_descriptor(handle, &desc_len);

    bool is_ups = false;
    HIDParser temp_parser;
    if (desc) {
        temp_parser.parseReportDescriptor(desc, desc_len);
        for (const auto& u : temp_parser.getUsages()) {
            if ((u.usage & 0xFFFF0000) == HID_PAGE_UPS || (u.usage & 0xFFFF0000) == HID_PAGE_BATTERY) {
                is_ups = true;
                break;
            }
        }
    }

    if (is_ups) {
        if (_hid_dev_handle != NULL) {
            // Its DISCONNECTED callback runs synchronously in here but only
            // queues an event, so reset the old device's state ourselves.
            hid_host_device_close(_hid_dev_handle);
            reset_device_state();
        }

        _hid_dev_handle = handle;
        _hid_parser = temp_parser;

        _cached_report_descriptor_hex = "";
        for (size_t i = 0; i < desc_len; i++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X", desc[i]);
            _cached_report_descriptor_hex += hex;
        }

        hid_host_dev_info_t dev_info;
        if (hid_host_get_device_info(handle, &dev_info) == ESP_OK) {
            _vid = dev_info.VID;
            _pid = dev_info.PID;

            if (_driver) { delete _driver; _driver = nullptr; }
            if (_vid == 0x051D) { _driver = new APCDriver(); }
            else if (_vid == 0x0764) { _driver = new CyberPowerDriver(); }
            else if (_vid == 0x0463) { _driver = new EatonDriver(); }
            else if (_vid == 0x0d9f) { _driver = new PowercomDriver(); }
            else if (_vid == 0x04D8 && (_pid == 0xD004 || _pid == 0xD005)) { _driver = new OpenUPSDriver(); }
            else if (_vid == 0x075D && _pid == 0x0300) { _driver = new GoldenMateDriver(); }
            else { _driver = new GenericDriver(); }

            _quirks = 0;
            for (int q = 0; UPS_QUIRKS[q].vid != 0; q++) {
                if (UPS_QUIRKS[q].vid == _vid && (UPS_QUIRKS[q].pid == 0xFFFF || UPS_QUIRKS[q].pid == _pid)) {
                    _quirks |= UPS_QUIRKS[q].flags;
                }
            }
            _driver->setup();
            populateStringsFromDeviceInfo(dev_info, _ups_data);
        }

        hid_host_device_start(_hid_dev_handle);
        _is_ready_to_poll = true;

        if (_log_cb) _log_cb("INFO", "UPS interface claimed and ready.");
    } else {
        // Not a UPS! Close it!
        if (_log_cb) _log_cb("INFO", "Ignoring non-UPS interface.");
        hid_host_device_close(handle);
    }
}

// Loop task only, under _mutex.
void USBHostUPS::reset_device_state() {
    std::lock_guard<std::recursive_mutex> io(_io_mutex);
    _hid_dev_handle = NULL;
    _is_ready_to_poll = false;
    // A new interface can reuse the old one's address, so the handle check in
    // drain_reports() cannot tell their reports apart.
    xQueueReset(_report_queue);
    _ups_data = UPSData();
    if (_driver) { delete _driver; _driver = nullptr; }
    _hid_parser = HIDParser();
}

bool USBHostUPS::isInterruptReport(uint8_t report_id) const {
    // Long enough to ride out a gap in a slow device's reporting, short enough
    // that polling resumes if the endpoint goes quiet for good.
    static const uint32_t INTERRUPT_REPORT_TTL_MS = 30000;
    auto it = _interrupt_report_seen.find(report_id);
    if (it == _interrupt_report_seen.end()) return false;
    return (millis() - it->second) < INTERRUPT_REPORT_TTL_MS;
}

bool USBHostUPS::requestReport(uint8_t report_id, uint8_t report_type, uint16_t expected_length) {
    uint8_t data[256];
    size_t length = expected_length > 0 ? expected_length : 255;

    // Devices that answer with the wrong report also truncate it to whatever the
    // requested report's length happens to be, so ask for the longest one.
    bool shifted = (_quirks & QUIRK_SHIFTED_REPORTS) != 0;
    if (shifted) {
        uint16_t max_len = _hid_parser.getMaxExpectedLength();
        if (max_len > length) length = max_len;
        if (length > sizeof(data)) length = sizeof(data);
    }

    esp_err_t err;
    {
        std::lock_guard<std::recursive_mutex> io(_io_mutex);
        if (!_is_ready_to_poll || !_hid_dev_handle) return false;
        err = hid_class_request_get_report(_hid_dev_handle, report_type, report_id, data, &length);
    }
    if (err == ESP_OK && length > 0) {
        uint8_t actual_id = report_id;
        uint8_t actual_type = report_type;
        _reports_received++;

        // Trust the ID the response carries over the one we asked for; decoding it
        // as the requested report would read every field from the wrong offsets.
        if (shifted && report_id != 0 && data[0] != report_id) {
            if (!_hid_parser.resolveReportType(data[0], report_type, actual_type)) {
                _reports_discarded++;
                return false;
            }
            _reports_rekeyed++;
            actual_id = data[0];
        }

        std::vector<uint8_t> payload(data, data + length);
        uint16_t key = (actual_type << 8) | actual_id;
        _cached_reports[key] = {actual_id, actual_type, payload};

        if (_driver) {
            _driver->decodeReport(this, actual_id, actual_type, data, length, _ups_data);
        }
        return true;
    } else {
        char dbg[128];
        if (err == 0x10c) {
            snprintf(dbg, sizeof(dbg), "requestReport FAILED: type=%d, id=%d, err=0x%x (STALL/NOT_FINISHED)", report_type, report_id, err);
        } else {
            snprintf(dbg, sizeof(dbg), "requestReport FAILED: type=%d, id=%d, err=0x%x", report_type, report_id, err);
        }
        if (_log_cb) _log_cb("ERROR", dbg);
    }
    return false;
}

bool USBHostUPS::requestStringDescriptor(uint8_t string_index) {
    return false;
}

IUSBHostUPS::UPSDataLock USBHostUPS::getUPSData() const {
    return IUSBHostUPS::UPSDataLock(_ups_data, this);
}

String USBHostUPS::getUPSStatusString() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    String status = UPSData::computeUPSStatusString(_ups_data);
    if (status.isEmpty()) {
        return "UNKNOWN";
    }
    return status;
}

void USBHostUPS::setLogCallback(LogCallback cb) {
    _log_cb = cb;
}

bool USBHostUPS::setBeeper(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    std::lock_guard<std::recursive_mutex> io(_io_mutex);
    if (!_is_ready_to_poll) return false;

    String active_path = getActiveBeeperPath();
    if (active_path == "") return false;
    
    const HIDUsageDef* def = nullptr;
    for (const auto& u : _hid_parser.getUsages()) {
        if (strcmp(u.path, active_path.c_str()) == 0) {
            def = &u;
            break;
        }
    }
    
    if (!def) {
        return false;
    }
    
    uint8_t rep_type = (def->report_type == 2) ? HID_REPORT_TYPE_OUTPUT : HID_REPORT_TYPE_FEATURE;
    
    uint16_t expected_length = _hid_parser.getExpectedLength(def->report_id, rep_type);
    
    uint8_t buffer[256];
    memset(buffer, 0, sizeof(buffer));
    size_t fetched_len = expected_length;
    
    // STEP 1: Fetch current report to preserve other fields
    esp_err_t err = hid_class_request_get_report(_hid_dev_handle, rep_type, def->report_id, buffer, &fetched_len);
    
    if (err != ESP_OK || fetched_len == 0) {
        // Fallback for UPSes that reject GET_REPORT on features
        fetched_len = expected_length;
        if (def->report_id != 0) buffer[0] = def->report_id;
    }
    
    fetched_len = BeeperLogic::manipulateBeeperBuffer(enable, def, buffer, fetched_len, _driver);
    if (fetched_len == 0) return false;
    
    // STEP 3: Write back
    err = hid_class_request_set_report(_hid_dev_handle, rep_type, def->report_id, buffer, fetched_len);
    
    // Update local state immediately to prevent UI bouncing
    if (err == ESP_OK) {
        _ups_data.set("ups.beeper.status", enable ? "enabled" : "disabled");
    }
    
    return err == ESP_OK;
}

bool USBHostUPS::isConnected() const {
    return _is_ready_to_poll;
}

String USBHostUPS::getActiveBeeperPath() const {
    for (const auto& u : _hid_parser.getUsages()) {
        if (strcmp(u.path, "UPS.PowerSummary.AudibleAlarmControl") == 0 || 
            strcmp(u.path, "UPS.BatterySystem.Battery.AudibleAlarmControl") == 0 || 
            strcmp(u.path, "UPS.AudibleAlarmControl") == 0) {
            return u.path;
        }
    }
    return "";
}

bool USBHostUPS::supportsBeeperToggle() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return getActiveBeeperPath() != "";
}

String USBHostUPS::dumpUSBDiagnostics() {
    JsonDocument doc;
    
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        doc["firmware_version"] = FIRMWARE_VERSION;
        doc["vid"] = String(_vid, HEX);
        doc["pid"] = String(_pid, HEX);
        doc["manufacturer"] = _ups_data.get("ups.mfr");
        doc["product"] = _ups_data.get("ups.model");
        doc["serial_number"] = _ups_data.get("ups.serial");
        
        // Format hex string as a custom 10-items-per-line JSON Array
        String arrStr = "[\n";
        if (_cached_report_descriptor_hex.length() > 0) {
            for (size_t i = 0; i < _cached_report_descriptor_hex.length(); i += 2) {
                if (i > 0 && (i / 2) % 10 == 0) arrStr += ",\n    ";
                else if (i > 0) arrStr += ", ";
                else arrStr += "    ";
                
                String byteStr = _cached_report_descriptor_hex.substring(i, i+2);
                byteStr.toUpperCase();
                arrStr += "\"0x" + byteStr + "\"";
            }
        }
        arrStr += "\n  ]";
        doc["report_descriptor_hex"] = serialized(arrStr);
        
        doc["quirks"] = _quirks;
        doc["driver"] = _driver ? _driver->getDriverName() : "None";

        JsonObject counters = doc["report_counters"].to<JsonObject>();
        counters["received"] = _reports_received;
        counters["rekeyed"] = _reports_rekeyed;
        counters["discarded"] = _reports_discarded;
        counters["interrupt_dropped"] = _interrupt_reports_dropped.load();
        counters["uptime_ms"] = millis();

        JsonArray scenarios = doc["scenarios"].to<JsonArray>();
        JsonObject scenario = scenarios.add<JsonObject>();
        scenario["description"] = "Live ESP32 dump";
        
        JsonArray reports = scenario["reports"].to<JsonArray>();
        for (const auto& kv : _cached_reports) {
            JsonObject report = reports.add<JsonObject>();
            report["id"] = kv.second.report_id;
            report["type"] = kv.second.report_type;
            
            String dataStr = "[";
            for (size_t i = 0; i < kv.second.data.size(); ++i) {
                if (i > 0) dataStr += ", ";
                dataStr += String(kv.second.data[i]);
            }
            dataStr += "]";
            report["data"] = serialized(dataStr);
        }
        
        JsonObject expectedData = scenario["expected_ups_data"].to<JsonObject>();
        for (const auto& param : _ups_data.getAll()) {
            expectedData[param.key] = param.value;
        }
    }

    String output;
    serializeJsonPretty(doc, output);
    return output;
}

void USBHostUPS::usb_host_lib_task(void *arg) {
    USBHostUPS *self = static_cast<USBHostUPS*>(arg);
    while (self->_usb_task_run) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (err == ESP_OK) {
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                // If there are no clients, we can do usb_host_uninstall() or just delay.
                // But hid_host is a client, so it's fine.
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else if (err == ESP_ERR_TIMEOUT) {
            // expected timeout
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    vTaskDelete(NULL);
}

void USBHostUPS::logDebug(const String& msg) const {
    if (_log_cb) _log_cb("DEBUG", msg.c_str());
}

void USBHostUPS::populateStringsFromDeviceInfo(const hid_host_dev_info_t& dev_info, UPSData& ups_data) {
    char buf[128];
    wcstombs(buf, dev_info.iManufacturer, sizeof(buf));
    if (String(buf).length() > 0) ups_data.set("ups.mfr", String(buf));
    
    wcstombs(buf, dev_info.iProduct, sizeof(buf));
    if (String(buf).length() > 0) ups_data.set("ups.model", String(buf));
    
    wcstombs(buf, dev_info.iSerialNumber, sizeof(buf));
    if (String(buf).length() > 0 && String(buf) != "Blank") ups_data.set("ups.serial", String(buf));
}

