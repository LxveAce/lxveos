// Host-side unit test for lxveos_wifi_audit (the pure evil-twin / rogue-AP classification). Pure libc, no
// ESP-IDF toolchain. Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assert.
// These pin the CURRENT predicate (nbssid>=2 or open+encrypted mix flags); the deliberate precision raise is
// a separate, separately-tested step.
#include "lxveos_wifi_audit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static lxveos_ap_view_t mk(const char *ssid, bool open)
{
    lxveos_ap_view_t v = {ssid, open, 0, 0, NULL};
    return v;
}

static void test_first_of_essid(void)
{
    lxveos_ap_view_t aps[] = {mk("", false), mk("Named", false), mk("Named", true)};
    // a hidden AP is never "first" (no name to group)
    assert(!lxveos_twin_first_of_essid(aps, 3, 0));
    // the first "Named" is first; the second occurrence is not
    assert(lxveos_twin_first_of_essid(aps, 3, 1));
    assert(!lxveos_twin_first_of_essid(aps, 3, 2));
    // out-of-range / NULL are safe
    assert(!lxveos_twin_first_of_essid(aps, 3, 9));
    assert(!lxveos_twin_first_of_essid(NULL, 3, 0));
}

static void test_analyze(void)
{
    // a single AP with a unique ESSID is not a twin
    lxveos_ap_view_t one[] = {mk("HomeNet", false)};
    lxveos_twin_verdict_t v = lxveos_twin_analyze(one, 1, "HomeNet");
    assert(v.nbssid == 1 && v.nopen == 0 && v.nenc == 1 && !v.flagged);

    // two encrypted BSSIDs sharing an ESSID -> flagged by the current nbssid>=2 rule
    lxveos_ap_view_t mesh[] = {mk("Mesh", false), mk("Mesh", false)};
    v = lxveos_twin_analyze(mesh, 2, "Mesh");
    assert(v.nbssid == 2 && v.nopen == 0 && v.nenc == 2 && v.flagged);

    // one open + one encrypted BSSID sharing an ESSID -> flagged (the karma/pineapple signature)
    lxveos_ap_view_t cafe[] = {mk("Cafe", true), mk("Cafe", false)};
    v = lxveos_twin_analyze(cafe, 2, "Cafe");
    assert(v.nbssid == 2 && v.nopen == 1 && v.nenc == 1 && v.flagged);

    // two distinct single-BSSID ESSIDs -> neither flagged; the analyze is scoped to the queried ESSID
    lxveos_ap_view_t two[] = {mk("A", false), mk("B", true)};
    assert(!lxveos_twin_analyze(two, 2, "A").flagged);
    v = lxveos_twin_analyze(two, 2, "B");
    assert(v.nbssid == 1 && v.nopen == 1 && v.nenc == 0 && !v.flagged);

    // a hidden AP never matches a real ESSID query (empty ssid won't strcmp-match)
    lxveos_ap_view_t hid[] = {mk("Real", false), mk("", false)};
    v = lxveos_twin_analyze(hid, 2, "Real");
    assert(v.nbssid == 1 && !v.flagged);

    // NULL / empty essid, or NULL aps -> all-zero unflagged
    v = lxveos_twin_analyze(two, 2, "");
    assert(v.nbssid == 0 && !v.flagged);
    v = lxveos_twin_analyze(two, 2, NULL);
    assert(v.nbssid == 0 && !v.flagged);
    v = lxveos_twin_analyze(NULL, 0, "A");
    assert(v.nbssid == 0 && !v.flagged);
}

int main(void)
{
    test_first_of_essid();
    test_analyze();
    printf("test_wifi_analytics: all assertions passed\n");
    return 0;
}
