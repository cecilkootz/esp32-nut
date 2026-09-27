#include "core/app_logger.h"
#include <stdarg.h>

LogMessage AppLogger::logBuffer[AppLogger::MAX_LOGS];
int AppLogger::head = 0;
int AppLogger::count = 0;
uint32_t AppLogger::nextId = 1;

// The loop task and the USB HID host task both log. Created during static
// initialisation, before any task can take it.
static StaticSemaphore_t ringLockBuffer;
static SemaphoreHandle_t ringLock = xSemaphoreCreateMutexStatic(&ringLockBuffer);

void AppLogger::log(const String& level, const String& msg) {
    emit(level.c_str(), msg.c_str());
}

void AppLogger::log(const char* level, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    emit(level, buffer);
}

void AppLogger::emit(const char* level, const char* msg) {
    // A String whose allocation failed has a null c_str().
    if (!level) level = "";
    if (!msg) msg = "";

    unsigned long now = millis();
    // Serial has its own lock; never nest it inside ours.
    Serial.printf("[%lu] [%s] %s\n", now, level, msg);

    xSemaphoreTake(ringLock, portMAX_DELAY);
    LogMessage& slot = logBuffer[head];
    slot.id = nextId++;
    slot.time = now;
    strlcpy(slot.level, level, sizeof(slot.level));
    strlcpy(slot.msg, msg, sizeof(slot.msg));
    head = (head + 1) % MAX_LOGS;
    if (count < MAX_LOGS) {
        count++;
    }
    xSemaphoreGive(ringLock);
}

String AppLogger::getLogsJSON() {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    // Serialised after the lock is released, so the document must own its
    // strings: ArduinoJson copies char arrays but keeps const ones by pointer.
    xSemaphoreTake(ringLock, portMAX_DELAY);
    int startIdx = (count < MAX_LOGS) ? 0 : head;
    for (int i = 0; i < count; i++) {
        int idx = (startIdx + i) % MAX_LOGS;
        JsonObject obj = array.add<JsonObject>();
        obj["id"] = logBuffer[idx].id;
        obj["time"] = logBuffer[idx].time;
        obj["level"] = logBuffer[idx].level;
        obj["msg"] = logBuffer[idx].msg;
    }
    xSemaphoreGive(ringLock);

    String output;
    serializeJson(doc, output);
    return output;
}
