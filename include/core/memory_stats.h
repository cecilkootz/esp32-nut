#ifndef MEMORY_STATS_H
#define MEMORY_STATS_H

#include <Arduino.h>

// Heap figures cover internal RAM. Stack figures are the least free stack each
// task has had since it started, in bytes.
struct MemoryStats {
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t largest_free_block;
    uint32_t loop_stack_min_free;
    int32_t poll_stack_min_free; // -1 if the ups_poll task is not running
    int32_t hid_stack_min_free;  // -1 if the HID host task is not running
};

// Call from the loop task: its stack is measured as the caller's.
MemoryStats readMemoryStats();

#endif // MEMORY_STATS_H
