// lxveos_monitor — see lxveos_monitor.h. Dependency-free passive-capture rotation planner, host-tested
// off-target (tests/host_c/test_monitor.c). libc-only, no allocation.
#include "lxveos_monitor.h"

size_t lxveos_monitor_plan(size_t n, uint32_t total_secs, uint32_t min_dwell,
                           lxveos_monitor_slot_t *out, size_t max)
{
    if (out == NULL || n == 0 || min_dwell == 0 || total_secs < min_dwell) {
        return 0;
    }
    // The budget can afford at most this many detectors at the minimum dwell.
    size_t capacity = (size_t)(total_secs / min_dwell);  // >= 1, since total_secs >= min_dwell
    size_t slots = n < capacity ? n : capacity;
    if (slots > max) {
        slots = max;
    }
    if (slots == 0) {
        return 0;
    }
    // Even split; because slots <= total/min_dwell, base = floor(total/slots) is always >= min_dwell.
    uint32_t base = total_secs / (uint32_t)slots;
    uint32_t rem = total_secs % (uint32_t)slots;  // spread the leftover a second at a time to the first slots
    for (size_t i = 0; i < slots; i++) {
        out[i].detector = (uint8_t)i;
        out[i].seconds = base + (i < (size_t)rem ? 1u : 0u);
    }
    return slots;
}
