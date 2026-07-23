// Host-side unit test for lxveos_monitor (the passive-capture rotation planner). Pure libc, no ESP-IDF. Built
// + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_monitor.h"

#include <assert.h>
#include <stdio.h>

static uint32_t total_of(const lxveos_monitor_slot_t *s, size_t n)
{
    uint32_t t = 0;
    for (size_t i = 0; i < n; i++) {
        t += s[i].seconds;
    }
    return t;
}

static void test_all_fit(void)
{
    lxveos_monitor_slot_t s[8];
    // 3 detectors, 30s budget, 5s min: an even 10s each, in order, using the whole budget
    size_t k = lxveos_monitor_plan(3, 30, 5, s, 8);
    assert(k == 3);
    assert(s[0].detector == 0 && s[1].detector == 1 && s[2].detector == 2);
    assert(s[0].seconds == 10 && s[1].seconds == 10 && s[2].seconds == 10);
    assert(total_of(s, k) == 30);
}

static void test_remainder_goes_to_early_slots(void)
{
    lxveos_monitor_slot_t s[8];
    // 3 detectors, 32s, 5s min: base 10, remainder 2 -> first two slots get 11, last gets 10
    size_t k = lxveos_monitor_plan(3, 32, 5, s, 8);
    assert(k == 3);
    assert(s[0].seconds == 11 && s[1].seconds == 11 && s[2].seconds == 10);
    assert(total_of(s, k) == 32);
}

static void test_budget_too_small_drops_detectors(void)
{
    lxveos_monitor_slot_t s[8];
    // 5 detectors, 12s, 5s min: only floor(12/5)=2 can meet the minimum, so 3 are dropped
    size_t k = lxveos_monitor_plan(5, 12, 5, s, 8);
    assert(k == 2);
    assert(s[0].seconds == 6 && s[1].seconds == 6);  // 12 split across the 2 scheduled
    for (size_t i = 0; i < k; i++) {
        assert(s[i].seconds >= 5);  // the min-dwell clamp holds for every scheduled slot
    }
    assert(total_of(s, k) == 12);
}

static void test_edges(void)
{
    lxveos_monitor_slot_t s[8];
    // a single detector takes the whole budget
    assert(lxveos_monitor_plan(1, 20, 5, s, 8) == 1 && s[0].seconds == 20);
    // no detectors -> no schedule
    assert(lxveos_monitor_plan(0, 20, 5, s, 8) == 0);
    // a budget below a single min-dwell can't schedule anyone
    assert(lxveos_monitor_plan(3, 4, 5, s, 8) == 0);
    // a zero min-dwell is invalid
    assert(lxveos_monitor_plan(3, 30, 0, s, 8) == 0);
    // the caller's out cap limits the plan (would be 5, capped to 3, still spending the whole budget)
    size_t k = lxveos_monitor_plan(5, 100, 5, s, 3);
    assert(k == 3 && total_of(s, k) == 100);
    // NULL out -> 0
    assert(lxveos_monitor_plan(3, 30, 5, NULL, 8) == 0);
}

int main(void)
{
    test_all_fit();
    test_remainder_goes_to_early_slots();
    test_budget_too_small_drops_detectors();
    test_edges();
    printf("test_monitor: all tests passed\n");
    return 0;
}
