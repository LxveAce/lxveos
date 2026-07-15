#pragma once
// LxveOS capability registry. A "capability" is a security-relevant subsystem — Wi-Fi, BLE, on-board
// storage, an add-on radio (CC1101/nRF24), etc. Each has a COMPILE-TIME bit, set from this board's
// CONFIG_LXVEOS_HAS_* (generated from cyd_boards.json), and a RUNTIME bit resolved by the boot probe.
//
// M0 seeds the runtime-active set straight from the compile-time set. Runtime-only add-ons (a CC1101 or
// PN532 detected on a bus, eFuse-gated silicon features) light up in M1 inside lxveos_caps_probe() with
// no rebuild and no change to callers. Feature code — the CLI, the LVGL UI, the Cyber Controller bridge —
// gates on lxveos_cap_active(), never on raw CONFIG_*, so this registry is the single source of truth for
// "what can this unit actually do right now".
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LXVEOS_CAP_WIFI = 0,
    LXVEOS_CAP_BLE,
    LXVEOS_CAP_BT_CLASSIC,
    LXVEOS_CAP_DISPLAY,
    LXVEOS_CAP_STORAGE,
    LXVEOS_CAP_GPS,
    LXVEOS_CAP_IR,
    LXVEOS_CAP_SUBGHZ,
    LXVEOS_CAP_NRF24,
    LXVEOS_CAP_NFC,
    // 5 GHz Wi-Fi is a distinct radio capability: the M0 fleet is ESP32/ESP32-S3 (2.4 GHz-only), so no board
    // sets CONFIG_LXVEOS_HAS_WIFI_5GHZ and this bit is never active — a 5 GHz op honestly reports "unavailable"
    // (this silicon can't) rather than "planned" (coming soon). A future dual-band board (ESP32-C5/C6) declares it.
    LXVEOS_CAP_WIFI_5GHZ,
    LXVEOS_CAP_COUNT
} lxveos_cap_t;

// A set of capabilities, one bit per lxveos_cap_t (LXVEOS_CAP_COUNT <= 32).
typedef uint32_t lxveos_cap_mask_t;

// The compile-time capability set: exactly the CONFIG_LXVEOS_HAS_* enabled for this board. Constant.
lxveos_cap_mask_t lxveos_caps_builtin(void);

// Run the boot capability probe and return the resulting active set. M0: active == compile-time set.
// M1 adds bus/eFuse-detected add-ons here. Idempotent.
lxveos_cap_mask_t lxveos_caps_probe(void);

// The active capability set resolved by the last probe (0 before the first probe runs).
lxveos_cap_mask_t lxveos_caps_active(void);

// True if `cap` is active per the last probe. This is the gate feature code should use.
bool lxveos_cap_active(lxveos_cap_t cap);

// Stable lowercase slug for a capability ("wifi", "ble", ...); "?" if out of range. For logs / CC bridge.
const char *lxveos_cap_name(lxveos_cap_t cap);

// The set of capabilities that are attachable ADD-ONs on this board — external modules on operator-supplied
// pins (CC1101 sub-GHz, nRF24, PN532 NFC, IR, GPS, SD): marked "addon" in cyd_boards.json, compiled from
// this board's CONFIG_LXVEOS_ADDON_*. Distinct from lxveos_caps_builtin() (soldered/silicon capabilities).
lxveos_cap_mask_t lxveos_caps_addon(void);

// True if `cap` is an attachable add-on on this board (in the addon mask). Lets feature code report an op the
// board can't do RIGHT NOW but could once the module is wired — an honest middle state between ready and
// fundamentally-unavailable. False for out-of-range caps.
bool lxveos_cap_is_addon(lxveos_cap_t cap);

#ifdef __cplusplus
}
#endif
