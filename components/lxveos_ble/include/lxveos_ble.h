// LxveOS BLE (M1). The recon side is a passive NimBLE GAP "observer": it listens for BLE advertisements
// and reports what is already in the air, using passive discovery (never sends the active SCAN_REQ that
// would prompt a scan response), so a scan puts nothing on-air.
//
// The component also hosts ONE offensive-TX op — arm-gated BLE HID keystroke injection (the `ble_hid_inject`
// / "BadBLE" op, see the lxveos_ble_hid_* API below). For that the connectable NimBLE PERIPHERAL role is
// compiled in; the non-connectable BROADCASTER role (what BLE advert-spam floods use) stays compiled out.
// Nothing transmits until an operator arms the unit and starts an injection — a plain scan never advertises.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One discovered BLE advertiser. `addr` is in NimBLE little-endian order (addr[0] is the least
// significant octet); print it reversed for the conventional MSB-first BLE MAC display.
typedef struct {
    uint8_t  addr[6];     // device address as reported by the controller (little-endian)
    uint8_t  addr_type;   // BLE_ADDR_* : 0 public, 1 random, 2 public-id (RPA), 3 random-id (RPA)
    int8_t   rssi;        // last-seen signal strength (dBm)
    uint8_t  adv_flags;   // GAP flags octet (AD type 0x01), valid only when flags_present
    bool     flags_present;
    uint8_t  name_len;    // bytes used in name[] (0 if the advert carried no local name)
    char     name[32];    // complete/short local name if advertised, NUL-terminated
    uint16_t company_id;  // manufacturer company ID (mfg_data[0..1], little-endian); valid if has_mfg
    bool     has_mfg;     // the advert carried manufacturer-specific data
    bool     fastpair;    // Google Fast Pair service-data (UUID 0xFE2C) present
    uint16_t appearance;  // GAP appearance value; valid only if appearance_present
    bool     appearance_present;
    uint16_t svc_uuids[8];   // advertised 16-bit service-class UUIDs (AD type 0x02/0x03)
    uint8_t  svc_uuid_count; // entries used in svc_uuids[] (0 if none advertised)
    bool     svc_uuids_partial; // the advert listed more 16-bit UUIDs than svc_uuids[] holds (truncated)
    uint8_t  tracker;        // item-tracker classification (LXVEOS_BLE_TRACKER_*); 0 = not a known tracker
} lxveos_ble_dev_t;

// Item-tracker classifications for lxveos_ble_dev_t.tracker. Signatures verified against the TU-Darmstadt
// AirGuard anti-stalking project: Apple Find My = mfg company 0x004C + payload type byte 0x12 (offline
// finding); Tile = service UUID 0xFEED; Samsung SmartTag = service UUID 0xFD5A; Chipolo = 0xFE33; PebbleBee
// = 0xFA25; Google Find My Network = 0xFEAA service DATA with frame byte 0x40 (the frame byte is what tells
// it apart from an Eddystone beacon, which also uses 0xFEAA — so an Eddystone is never mislabelled).
#define LXVEOS_BLE_TRACKER_NONE        0
#define LXVEOS_BLE_TRACKER_APPLE_FINDMY 1  // AirTag / Find My accessory in offline-finding state
#define LXVEOS_BLE_TRACKER_TILE        2
#define LXVEOS_BLE_TRACKER_SMARTTAG    3  // Samsung Galaxy SmartTag (SmartThings Find)
#define LXVEOS_BLE_TRACKER_CHIPOLO     4
#define LXVEOS_BLE_TRACKER_PEBBLEBEE   5
#define LXVEOS_BLE_TRACKER_GOOGLE_FMN  6  // Google Find My Network (0xFEAA svc-data frame 0x40, not Eddystone)

// Run a PASSIVE BLE GAP discovery for `seconds` (clamped to a sane default if 0), collecting up to `max`
// unique advertisers into `out`. De-dups by address (RSSI/name refresh on repeat sightings). On return
// *found holds the number of unique devices written (<= max). LISTEN ONLY — never advertises, never sends
// a scan request. Brings the NimBLE host up lazily on first call and leaves it up for subsequent scans.
// Returns ESP_OK on a clean scan, or an esp_err_t on init/discovery failure.
esp_err_t lxveos_ble_scan(uint32_t seconds, lxveos_ble_dev_t *out, size_t max, size_t *found);

// Result of a passive BLE advertisement-flood / spam watch (see lxveos_ble_flood_watch). The detector
// keys on advertiser churn: a BLE-spam / advert-flood attack (Flipper "BLE Spam", Apple/Android/Windows
// popup floods) rotates through a torrent of distinct — usually random — addresses, while a normal room
// shows a small, stable set of advertisers.
typedef struct {
    uint32_t seconds;         // window watched
    uint32_t total_adv;       // every advertisement report seen (repeats included)
    uint32_t unique_addrs;    // distinct advertiser addresses observed (capped; see unique_overflow)
    bool     unique_overflow; // the distinct-address table filled — itself a strong flood signal
    uint32_t random_addrs;    // how many of the unique advertisers used a random address type
    uint8_t  top_addr[6];     // busiest single advertiser (most advertisements), little-endian
    uint8_t  top_addr_type;   // its address type (BLE_ADDR_*)
    uint32_t top_count;       // advertisements from that busiest advertiser
    // Advert-payload vendor tally (counts adverts, not unique addresses). A BLE-spam flood's payloads
    // cluster on one vendor, so the dominant counter attributes the flood.
    uint32_t v_apple;         // manufacturer company 0x004C (Apple continuity/proximity)
    uint32_t v_microsoft;     // 0x0006 (Microsoft Swift Pair)
    uint32_t v_google;        // 0x00E0 (Google)
    uint32_t v_samsung;       // 0x0075 (Samsung)
    uint32_t v_fastpair;      // Google Fast Pair service-data UUID 0xFE2C
    uint32_t v_other_mfg;     // manufacturer data present, company not in the known set
} lxveos_ble_flood_stats_t;

// Watch the air for `seconds` (default if 0) with a PASSIVE GAP discovery and measure advertiser churn
// into *out. LISTEN ONLY — advertises nothing, sends no scan requests. Returns ESP_OK on a clean watch.
esp_err_t lxveos_ble_flood_watch(uint32_t seconds, lxveos_ble_flood_stats_t *out);

// Human-readable name for a BLE address type (public / random / pub-id / rnd-id / ?).
const char *lxveos_ble_addr_type_str(uint8_t addr_type);

// Short vendor name for a BLE company identifier (Apple/Microsoft/Google/Samsung/Nordic/Garmin), or NULL
// if the ID is not in the known set. Used to attribute advertisers + BLE-spam floods to a vendor.
const char *lxveos_ble_company_name(uint16_t company_id);

// Short name for a common 16-bit BLE service-class UUID (Battery / HeartRate / HID / FastPair / Eddystone /
// …), or NULL if the UUID is not in the known set — the caller then shows the raw 0xNNNN (honesty gate,
// same policy as the company table: named only when certain, never mislabelled).
const char *lxveos_ble_service_name(uint16_t uuid16);

// Short human label for an item-tracker classification (LXVEOS_BLE_TRACKER_*): "AirTag/FindMy", "Tile",
// "SmartTag", or NULL for LXVEOS_BLE_TRACKER_NONE. Used by the passive tracker/stalking detector.
const char *lxveos_ble_tracker_str(uint8_t tracker);

// Write a short human label for a GAP appearance value into `buf` (category = value >> 6): common consumer
// categories (Phone/Watch/Computer/Audio/Heart-Rate/…) are named, HID (cat 15) resolves its keyboard/mouse
// subcategory, and anything not in the known set falls through to "appr:0x<hex>" — never mis-labelled.
void lxveos_ble_appearance_str(uint16_t appearance, char *buf, size_t buflen);

// If `d` is a Flipper Zero, return its case colour ("Black"/"White"/"Transparent"); else NULL. Detection is a
// match on the advertised 16-bit service UUIDs 0x3081/0x3082/0x3083 (parsed into d->svc_uuids). Ported from
// ESP32 Marauder "Flipper Sniff" (MIT — see CREDITS.md). Pure over the scanned device — host-tested.
const char *lxveos_ble_flipper_color(const lxveos_ble_dev_t *d);

// ── BLE HID keystroke injection (the `ble_hid_inject` op, "BadBLE") — OFFENSIVE TX, arm-gated ─────────
// LxveOS advertises as a standard BLE HID keyboard ("LxveOS-KB"); when a target host pairs and subscribes
// to the input report, it plays `script` as keystrokes — a Rubber-Ducky primitive over BLE, for authorized
// lab testing. Targeted injection into the one host that connects; NOT a jammer or advert-spam flood.
//
// `script` is a DuckyScript-lite string, commands separated by ';':
//   STRING <text>  type literal text        DELAY <ms>   pause
//   ENTER TAB ESC SPACE BACKSPACE DELETE UP DOWN LEFT RIGHT HOME END   named keys
//   GUI <k> · CTRL <k> · ALT <k> · SHIFT <k> · CTRL-ALT <k> · GUI      modifier + key (key optional)
// e.g. "GUI r;DELAY 400;STRING notepad;ENTER".
//
// Requires the unit to be ARMED (lxveos_arm_can_emit()). Returns ESP_ERR_NOT_ALLOWED if not armed / TX
// compiled out, ESP_ERR_INVALID_STATE if already running, ESP_ERR_INVALID_ARG for an empty script, or an
// esp_err_t on BLE bring-up failure. On success it advertises and waits up to 60 s for a target; the script
// plays once the target subscribes, then it auto-stops.
esp_err_t lxveos_ble_hid_inject(const char *script);

// Stop advertising / disconnect / end any in-flight injection. Safe to call when idle (returns ESP_OK).
esp_err_t lxveos_ble_hid_stop(void);

// True while advertising as a keyboard or injecting. / True once a host has connected.
bool lxveos_ble_hid_running(void);
bool lxveos_ble_hid_connected(void);

#ifdef __cplusplus
}
#endif
