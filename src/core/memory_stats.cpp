#include "core/memory_stats.h"

MemoryStats readMemoryStats() {
    // Looked up once: the search walks every task list, and both tasks live until
    // USBHostUPS::end(), which only runs just before a restart. The HID library
    // keeps its task handle private.
    static TaskHandle_t hid_task = NULL;
    static TaskHandle_t poll_task = NULL;
    if (!hid_task) {
        hid_task = xTaskGetHandle("USB HID Host");
    }
    if (!poll_task) {
        poll_task = xTaskGetHandle("ups_poll");
    }

    MemoryStats stats;
    stats.free_heap = ESP.getFreeHeap();
    stats.min_free_heap = ESP.getMinFreeHeap();
    stats.largest_free_block = ESP.getMaxAllocHeap();
    stats.loop_stack_min_free = uxTaskGetStackHighWaterMark(NULL);
    stats.poll_stack_min_free = poll_task ? (int32_t)uxTaskGetStackHighWaterMark(poll_task) : -1;
    stats.hid_stack_min_free = hid_task ? (int32_t)uxTaskGetStackHighWaterMark(hid_task) : -1;
    return stats;
}
