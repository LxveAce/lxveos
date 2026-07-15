#pragma once
// Internal BLE signature constants shared between lxveos_ble.c (the NimBLE observer + item-tracker
// classifier) and lxveos_ble_labels.c (the pure company/service label tables). Bluetooth SIG company IDs,
// common 16-bit service UUIDs, and the item-tracker advert signatures — the tracker ones verified against
// the TU-Darmstadt AirGuard anti-stalking project. Kept out of the public lxveos_ble.h: implementation detail.
#include <stdint.h>

// Company identifiers (manufacturer-specific data, little-endian first two bytes).
#define LXVEOS_BLE_CID_APPLE     0x004Cu
#define LXVEOS_BLE_CID_MICROSOFT 0x0006u
#define LXVEOS_BLE_CID_GOOGLE    0x00E0u
#define LXVEOS_BLE_CID_SAMSUNG   0x0075u

// 16-bit service UUIDs / advert signatures.
#define LXVEOS_BLE_SVC_FASTPAIR  0xFE2Cu  // Google Fast Pair service-data UUID
#define LXVEOS_BLE_APPLE_TYPE_FINDMY 0x12u  // Apple mfg-data payload type byte = Offline Finding (Find My)
#define LXVEOS_BLE_SVC_TILE          0xFEEDu // Tile trackers
#define LXVEOS_BLE_SVC_SMARTTAG      0xFD5Au // Samsung Galaxy SmartTag (SmartThings Find)
#define LXVEOS_BLE_SVC_CHIPOLO       0xFE33u // Chipolo trackers
#define LXVEOS_BLE_SVC_PEBBLEBEE     0xFA25u // PebbleBee trackers
#define LXVEOS_BLE_SVC_GOOGLE_FMN    0xFEAAu // Google Find My Network (service DATA; shares 0xFEAA w/ Eddystone)
#define LXVEOS_BLE_GOOGLE_FMN_FRAME  0x40u   // FMN service-data frame type — distinguishes it from Eddystone
                                             // beacons (frame types 0x00/0x10/0x20/0x30), avoiding false labels

// Flipper Zero BLE service UUIDs (16-bit, AD type 0x02/0x03) — one per case colour. ESP32 Marauder "Flipper
// Sniff" matches these bytes; LxveOS matches them in the already-parsed svc_uuids[] (a proper AD-structure
// match, tighter than Marauder's raw-payload byte scan). Flipper's mfg/company ID 0x0FBA is not used here.
#define LXVEOS_BLE_UUID_FLIPPER_BLACK       0x3081u
#define LXVEOS_BLE_UUID_FLIPPER_WHITE       0x3082u
#define LXVEOS_BLE_UUID_FLIPPER_TRANSPARENT 0x3083u

// Meta / Ray-Ban Meta glasses + Oculus (ESP32 Marauder "Meta Detect" / BT_SCAN_RAYBAN). A device matches if
// any of its manufacturer company ID or advertised 16-bit service UUID is in the Meta set — UNLESS one of its
// identifiers is in the deny-list first (blocked wins), which strips the Apple/Samsung/Microsoft popup-flood
// payloads. Values are Marauder's (WiFiScan.h ~815-830); 0xFD5F is Oculus VR (Meta) per the BT SIG registry.
#define LXVEOS_BLE_META_ID_0  0xFD5Fu  // Meta / Oculus VR
#define LXVEOS_BLE_META_ID_1  0xFEB7u  // Meta
#define LXVEOS_BLE_META_ID_2  0xFEB8u  // Meta
#define LXVEOS_BLE_META_ID_3  0x01ABu  // Meta
#define LXVEOS_BLE_META_ID_4  0x058Eu  // Meta
#define LXVEOS_BLE_META_ID_5  0x0D53u  // Luxottica (Ray-Ban Meta)
// Deny-list (checked first). 0xFD5A/0x004C/0x0006 already exist above as SVC_SMARTTAG / CID_APPLE / CID_MICROSOFT.
#define LXVEOS_BLE_META_BLOCK_SAMSUNG2 0xFD69u  // Samsung
#define LXVEOS_BLE_META_BLOCK_PHONE    0xFEF3u  // phone popup payload

// Flock Safety "Penguin" battery/camera (ESP32 Marauder "Flock Sniff" / isFlockCamera()). LxveOS ships only the
// SPECIFIC BLE signal: the XUNTONG manufacturer ID that Flock's hardware advertises, gated by a confirming Flock
// name pattern. Marauder's broad 27-entry OUI list (incl. the CC:CC:CC placeholder) and its generic Wi-Fi SSID
// substrings ("flock"/"penguin"/"pigvision") are DELIBERATELY NOT carried — too false-positive-prone to ever call
// a confident Flock ID. Value from WiFiScan.cpp isFlockCamera() (~1558-1640). Name patterns handled in labels.c.
#define LXVEOS_BLE_CID_XUNTONG 0x09C8u  // Flock "Penguin" battery/camera mfg company ID (the strongest BLE signal)
