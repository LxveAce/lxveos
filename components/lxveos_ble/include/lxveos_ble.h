// LxveOS BLE recon (M1). A passive NimBLE GAP "observer": it listens for BLE advertisements and reports
// what is already in the air. It is LISTEN-ONLY by construction — the broadcaster/peripheral (advertise/
// connectable) NimBLE roles are compiled OUT (see sdkconfig.defaults), so an LxveOS image is physically
// incapable of transmitting a BLE packet. The scan itself uses passive discovery (never sends the active
// SCAN_REQ that would prompt a scan response), so nothing leaves the antenna.
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
    uint8_t addr[6];      // device address as reported by the controller (little-endian)
    uint8_t addr_type;    // BLE_ADDR_* : 0 public, 1 random, 2 public-id (RPA), 3 random-id (RPA)
    int8_t  rssi;         // last-seen signal strength (dBm)
    uint8_t adv_flags;    // GAP flags octet (AD type 0x01), valid only when flags_present
    bool    flags_present;
    uint8_t name_len;     // bytes used in name[] (0 if the advert carried no local name)
    char    name[32];     // complete/short local name if advertised, NUL-terminated
} lxveos_ble_dev_t;

// Run a PASSIVE BLE GAP discovery for `seconds` (clamped to a sane default if 0), collecting up to `max`
// unique advertisers into `out`. De-dups by address (RSSI/name refresh on repeat sightings). On return
// *found holds the number of unique devices written (<= max). LISTEN ONLY — never advertises, never sends
// a scan request. Brings the NimBLE host up lazily on first call and leaves it up for subsequent scans.
// Returns ESP_OK on a clean scan, or an esp_err_t on init/discovery failure.
esp_err_t lxveos_ble_scan(uint32_t seconds, lxveos_ble_dev_t *out, size_t max, size_t *found);

// Human-readable name for a BLE address type (public / random / pub-id / rnd-id / ?).
const char *lxveos_ble_addr_type_str(uint8_t addr_type);

#ifdef __cplusplus
}
#endif
