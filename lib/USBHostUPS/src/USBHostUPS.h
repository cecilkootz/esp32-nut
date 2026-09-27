#ifndef USB_HOST_UPS_H
#define USB_HOST_UPS_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>
#include <mutex>
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "Quirks.h"
#include "HIDParser.h"
#include "UPSData.h"
#include "IUSBHostUPS.h"
#include <map>
#include <vector>

struct CachedReport {
    uint8_t report_id;
    uint8_t report_type;
    std::vector<uint8_t> data;
};

typedef void (*LogCallback)(const char* level, const char* msg);

class IUPSDriver;

class USBHostUPS : public IUSBHostUPS {
public:
    USBHostUPS();
    ~USBHostUPS();

    bool begin();
    void end();
    void loop();

    void lock() const override { _mutex.lock(); }
    void unlock() const override { _mutex.unlock(); }

    UPSDataLock getUPSData() const override;
    String getUPSStatusString() const override;
    String dumpUSBDiagnostics();

    bool setBeeper(bool enable) override;
    bool isConnected() const override;
    bool supportsBeeperToggle() const override;
    
    void setLogCallback(LogCallback cb);
    void logDebug(const String& msg) const override;

    const std::vector<HIDUsageDef>& getUsages() const override { return _hid_parser.getUsages(); }
    const HIDUsageDef* getUsageDef(uint32_t usage) const override { return _hid_parser.getUsageDef(usage); }
    const HIDParser* getHIDParser() const override { return &_hid_parser; }
    String getActiveBeeperPath() const override;
    uint32_t getQuirks() const override { return _quirks; }
    bool isControlPending() const override { return _control_pending; }
    bool isInterruptReport(uint8_t report_id) const override;
    bool requestReport(uint8_t report_id, uint8_t report_type, uint16_t expected_length = 8) override;
    bool requestStringDescriptor(uint8_t string_index) override;
    uint16_t getVID() const override { return _vid; }
    uint16_t getPID() const override { return _pid; }

    HIDParser _hid_parser;

public:
    static void populateStringsFromDeviceInfo(const hid_host_dev_info_t& dev_info, UPSData& ups_data);
private:
    mutable std::recursive_mutex _mutex;
    // Held by the loop task across every call into the HID library that takes a
    // device handle, and by the HID task while it closes a departed interface:
    // the library frees that interface's device as soon as the callback returns.
    std::recursive_mutex _io_mutex;
    static void hid_host_driver_event_cb(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg);
    static void hid_host_interface_event_cb(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg);
    static void control_transfer_cb(usb_transfer_t *transfer);
    static void usb_host_lib_task(void *arg);
    TaskHandle_t _usb_task_handle;
    volatile bool _usb_task_run;
    
    void handle_driver_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event);
    void handle_interface_event(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event);

    // The HID task also completes the loop task's control transfers, so its
    // callbacks only copy into these queues and loop() does the rest.
    struct InputReport {
        hid_host_device_handle_t handle;
        uint8_t len;
        uint8_t data[64]; // one full-speed interrupt packet, the most the library copies out
    };
    struct HidEvent {
        enum Kind : uint8_t { CONNECTED, OPEN_FAILED, DISCONNECTED } kind;
        hid_host_device_handle_t handle;
    };
    QueueHandle_t _report_queue = NULL;
    QueueHandle_t _event_queue = NULL;
    std::atomic<uint32_t> _interrupt_reports_dropped{0};
    std::atomic<uint32_t> _events_dropped{0};
    uint32_t _events_dropped_logged = 0;

    void post_event(HidEvent::Kind kind, hid_host_device_handle_t handle);
    void drain_reports();
    void process_input_report(const InputReport& report);
    void cache_report(uint8_t report_id, uint8_t report_type, const uint8_t* data, size_t length);
    void drain_events();
    void claim_interface(hid_host_device_handle_t handle);
    void reset_device_state();

    hid_host_device_handle_t _hid_dev_handle;
    usb_device_handle_t _dev_handle;
    
    String _cached_report_descriptor_hex;
    uint16_t _vid;
    uint16_t _pid;
    bool _initialized;
    // Also cleared by the HID task when the active interface departs.
    std::atomic<bool> _is_ready_to_poll;
    volatile bool _control_pending;

    UPSData _ups_data;
    IUPSDriver* _driver;
    LogCallback _log_cb;
    
    std::map<uint16_t, CachedReport> _cached_reports;
    // millis() of the last unsolicited interrupt report seen per report ID.
    std::map<uint8_t, uint32_t> _interrupt_report_seen;

    // Since boot: GET_REPORT responses received, how many carried a different
    // report ID than requested, and how many carried an ID the descriptor does
    // not declare. Read as a ratio; a rising rekeyed/received means the device
    // is currently answering out of step.
    uint32_t _reports_received = 0;
    uint32_t _reports_rekeyed = 0;
    uint32_t _reports_discarded = 0;

    // Last interrupt report shape logged, so a steady stream stays quiet.
    uint8_t _last_logged_interrupt_id = 0;
    size_t _last_logged_interrupt_len = 0;

    uint32_t _quirks;
};

#endif // USB_HOST_UPS_H
