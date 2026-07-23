#pragma once
// lxveos_monitor — dependency-free (libc-only) rotation planner for a `monitor` command: given a set of
// enabled passive detectors, a total time budget, and a per-detector minimum dwell, it produces a
// deterministic schedule (which detector runs, for how long, in order). Pure logic, so it host-tests
// off-target (tests/host_c/test_monitor.c). The runtime `monitor` command just runs the existing passive ops
// in the planned order and forwards their alert events; only this planner is here.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One slot in the rotation schedule.
typedef struct {
    uint8_t  detector;  // index (0..n-1) into the caller's enabled-detector list
    uint32_t seconds;   // dwell time for this slot (always >= the min_dwell passed to the planner)
} lxveos_monitor_slot_t;

// Split `total_secs` across `n` detectors into a deterministic rotation written to out[0..max). Every
// scheduled slot runs at least `min_dwell` seconds; if the budget cannot give all `n` detectors that minimum,
// only the first floor(total_secs / min_dwell) are scheduled (the rest are dropped). The whole budget is used:
// the even split's remainder is added one second at a time to the earliest slots. Returns the slot count
// (<= min(n, max)), or 0 for n == 0, min_dwell == 0, total_secs < min_dwell, or a NULL out.
size_t lxveos_monitor_plan(size_t n, uint32_t total_secs, uint32_t min_dwell,
                           lxveos_monitor_slot_t *out, size_t max);

#ifdef __cplusplus
}
#endif
