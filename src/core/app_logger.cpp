#include "core/app_logger.h"
#include <stdarg.h>
#include <mutex>

// The loop task and the USB host task both log (review A5b)
static std::recursive_mutex s_log_mutex;

LogMessage AppLogger::logBuffer[AppLogger::MAX_LOGS];
int AppLogger::head = 0;
int AppLogger::count = 0;
uint32_t AppLogger::nextId = 1;
AppLogger::Forwarder AppLogger::forwarder = nullptr;
void* AppLogger::forwarderCtx = nullptr;

void AppLogger::setForwarder(Forwarder fn, void* ctx) {
    std::lock_guard<std::recursive_mutex> lock(s_log_mutex);
    forwarder = fn;
    forwarderCtx = ctx;
}

void AppLogger::log(const String& level, const String& msg) {
    emit(level.c_str(), msg.c_str(), false);
}

void AppLogger::logNotice(const char* level, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    emit(level, buffer, true);
}

void AppLogger::log(const char* level, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    emit(level, buffer, false);
}

void AppLogger::emit(const char* level, const char* msg, bool notice) {
    // A String whose allocation failed has a null c_str().
    if (!level) level = "";
    if (!msg) msg = "";

    // One millis() for the serial line, the buffer and the remote outputs (US-118)
    const uint32_t now = millis();
    // Serial has its own lock; never nest it inside ours.
    Serial.printf("[%lu] [%s] %s\n", (unsigned long)now, level, msg);

    std::lock_guard<std::recursive_mutex> lock(s_log_mutex);
    LogMessage& slot = logBuffer[head];
    slot.id = nextId++;
    slot.time = now;
    strlcpy(slot.level, level, sizeof(slot.level));
    strlcpy(slot.msg, msg, sizeof(slot.msg));
    head = (head + 1) % MAX_LOGS;
    if (count < MAX_LOGS) {
        count++;
    }

    // Remote outputs: a non-blocking copy into the log queue (issue #69)
    if (forwarder) forwarder(forwarderCtx, level, msg, notice, now);
}

String AppLogger::getLogsJSON(BoardTimeFn fn, void* ctx) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    char board_time[40];

    // Serialised after the lock is released, so the document must own its
    // strings: ArduinoJson copies char arrays but keeps const ones by pointer.
    {
        std::lock_guard<std::recursive_mutex> lock(s_log_mutex);
        // After the lock: every line is written under it, so none is newer (US-118)
        const uint32_t list_uptime = millis();
        int startIdx = (count < MAX_LOGS) ? 0 : head;
        for (int i = 0; i < count; i++) {
            int idx = (startIdx + i) % MAX_LOGS;
            JsonObject obj = array.add<JsonObject>();
            obj["id"] = logBuffer[idx].id;
            obj["time"] = logBuffer[idx].time;
            obj["level"] = logBuffer[idx].level;
            obj["msg"] = logBuffer[idx].msg;
            if (fn && fn(ctx, (uint32_t)logBuffer[idx].time, list_uptime, board_time, sizeof(board_time))) {
                // A copy: the buffer is reused for the next line
                obj["board_time"] = String(board_time);
            }
        }
    }

    String output;
    serializeJson(doc, output);
    return output;
}
