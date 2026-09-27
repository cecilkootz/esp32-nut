#include "WebApiJson.h"

namespace {

// ArduinoJson's String writer frees the target's buffer first, discarding any reserve().
class StringAppender {
public:
    explicit StringAppender(String& out) : out_(out) {}
    size_t write(uint8_t c) { return write(&c, 1); }
    size_t write(const uint8_t* s, size_t n) { return out_.concat(s, n) ? n : 0; }

private:
    String& out_;
};

String toJsonString(const JsonDocument& doc) {
    String out;
    out.reserve(measureJson(doc));
    StringAppender appender(out);
    serializeJson(doc, appender);
    return out;
}

}  // namespace

String WebApiJson::generateUpsVars(IUSBHostUPS* usb_ups) {
    if (!usb_ups) {
        return "{\"error\": \"UPS non inizializzato\"}";
    }
    
    JsonDocument doc;
    
    if (!usb_ups->isConnected()) {
        doc["_disconnected"] = true;
        doc["ups.status"] = "Disconnected";
        return toJsonString(doc);
    }
    
    auto data = usb_ups->getUPSData();

    doc["ups.status"] = usb_ups->getUPSStatusString();
    if (usb_ups->isDataStale()) {
        doc["_stale"] = true;
    }
    
    for (const auto& param : data->getAll()) {
        if (param.key.startsWith("ups.status.") && param.key != "ups.status") {
            continue; // Skip internal status flags
        }
        
        bool isNumeric = true;
        bool hasDot = false;
        if (param.value.length() == 0) isNumeric = false;
        for (int i = 0; i < param.value.length(); i++) {
            if (i == 0 && param.value[i] == '-') continue;
            if (param.value[i] == '.') {
                if (hasDot) { isNumeric = false; break; }
                hasDot = true;
                continue;
            }
            if (!isdigit(param.value[i])) {
                isNumeric = false;
                break;
            }
        }
        
        if (isNumeric) {
            if (hasDot) doc[param.key] = serialized(param.value); // Use serialized for floats to preserve formatting
            else doc[param.key] = param.value.toInt();
        } else {
            doc[param.key] = param.value;
        }
    }
    
    doc["ups.beeper.switchable"] = usb_ups->supportsBeeperToggle();

    return toJsonString(doc);
}
