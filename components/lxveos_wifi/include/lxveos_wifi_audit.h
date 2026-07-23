#pragma once
// lxveos_wifi_audit — PURE Wi-Fi scan analytics (evil-twin / rogue-AP classification, and room for the other
// AP-audit predicates), split out of the CLI so the classification is host-tested off-target
// (tests/host_c/test_wifi_analytics.c). libc-only, no ESP-IDF. A caller builds an array of lxveos_ap_view_t
// from a scan (precomputing `open` via lxveos_wifi_is_open) and passes it in.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A minimal, ESP-IDF-free view of one scanned AP for the analytics. `ssid` is the NUL-terminated ESSID ("" if
// hidden); `open` is the precomputed lxveos_wifi_is_open(authmode); channel/rssi/bssid back the finer twin
// checks. The `ssid`/`bssid` pointers reference the caller's scan buffer and must outlive the call.
typedef struct {
    const char    *ssid;
    bool           open;
    uint8_t        channel;
    int8_t         rssi;
    const uint8_t *bssid;  // 6 bytes
} lxveos_ap_view_t;

// Per-ESSID evil-twin verdict computed across a scan.
typedef struct {
    int  nbssid;   // BSSIDs advertising this ESSID
    int  nopen;    // ... that are open
    int  nenc;     // ... that are encrypted
    bool flagged;  // a twin signature: more than one BSSID for the ESSID, or a mix of open and encrypted
} lxveos_twin_verdict_t;

// True if aps[idx] is the FIRST (lowest-index) AP advertising its ESSID, so a caller reports each distinct
// ESSID once. A hidden AP (empty ssid) is never "first" (returns false) — there is no name to group by.
bool lxveos_twin_first_of_essid(const lxveos_ap_view_t *aps, size_t n, size_t idx);

// Count the BSSIDs advertising `essid` across aps[0..n), split open vs encrypted, and set `flagged` if the
// group shows a twin signature (more than one BSSID, or a mix of open and encrypted BSSIDs). A NULL/empty
// essid or NULL aps yields an all-zero, unflagged verdict.
lxveos_twin_verdict_t lxveos_twin_analyze(const lxveos_ap_view_t *aps, size_t n, const char *essid);

#ifdef __cplusplus
}
#endif
