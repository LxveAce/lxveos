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
