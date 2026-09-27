#include "core/memory_stats.h"

MemoryStats readMemoryStats() {
    // The HID library keeps its task handle private. Look it up once: the search
    // walks every task list, and the task lives until hid_host_uninstall(), which
    // only runs just before a restart.
    static TaskHandle_t hid_task = NULL;
    if (!hid_task) {
        hid_task = xTaskGetHandle("USB HID Host");
    }

    MemoryStats stats;
    stats.free_heap = ESP.getFreeHeap();
    stats.min_free_heap = ESP.getMinFreeHeap();
    stats.largest_free_block = ESP.getMaxAllocHeap();
    stats.loop_stack_min_free = uxTaskGetStackHighWaterMark(NULL);
    stats.hid_stack_min_free = hid_task ? (int32_t)uxTaskGetStackHighWaterMark(hid_task) : -1;
    return stats;
}
