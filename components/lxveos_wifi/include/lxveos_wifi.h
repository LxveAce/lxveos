#pragma once
// LxveOS Wi-Fi recon (M1). The first real driver behind the operation catalog: a PASSIVE Access-Point
// scan. It brings the Wi-Fi stack up in station mode and listens for beacons only — WIFI_SCAN_TYPE_PASSIVE
// transmits nothing (no probe requests, and categorically no deauth/beacon/jammer frames — LxveOS never
// authors those). It never associates to a network. This is the honest, lawful recon primitive the
// `wifi_ap_scan` catalog row promises; higher-level features (station scan, EAPOL capture) build on it.
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One scanned access point. `ssid` is NUL-terminated (empty string for a hidden/beacon-cloaked AP).
typedef struct {
    char ssid[33];
    int8_t rssi;        // dBm
    uint8_t channel;    // primary channel
    uint8_t bssid[6];   // AP MAC
    uint8_t authmode;   // wifi_auth_mode_t
} lxveos_wifi_ap_t;

// Run one passive AP scan. Brings the Wi-Fi stack up (STA, no association) on first call and reuses it
// after. Blocks until the scan completes (~1-2 s across the channel plan). Copies up to `max` results into
// `out` and sets *found to the number copied. Returns ESP_OK on success, or an esp_err_t on failure (in
// which case *found is 0). Transmits no frames.
esp_err_t lxveos_wifi_scan(lxveos_wifi_ap_t *out, size_t max, size_t *found);

// Tally from one packet-monitor session. Frame counts are split by 802.11 type; per_channel[1..13] holds
// the count seen on each 2.4 GHz channel (index 0 unused).
typedef struct {
    uint32_t total;
    uint32_t mgmt;         // beacons, probe req/resp, assoc, auth, deauth, ... (management frames)
    uint32_t ctrl;         // RTS/CTS/ACK/block-ack (control frames)
    uint32_t data;         // data frames (incl. QoS)
    uint32_t misc;         // anything the driver reports as MISC
    uint32_t per_channel[14];
    uint8_t channels_swept;  // number of channel dwells performed
} lxveos_wifi_sniff_stats_t;

// Run a PASSIVE packet monitor for ~`seconds` (0 -> a default), channel-hopping across the 2.4 GHz plan
// in promiscuous mode. Listens only — enables promiscuous RX, transmits NOTHING (no probe/deauth/beacon).
// Brings the Wi-Fi stack up on first use. Blocks for the duration, then disables promiscuous and writes
// the tally to *out. Returns ESP_OK or an esp_err_t (out zeroed on failure). Captures no payloads/PII —
// only per-type / per-channel counts.
esp_err_t lxveos_wifi_sniff(uint32_t seconds, lxveos_wifi_sniff_stats_t *out);

// Short human label for a wifi_auth_mode_t value ("open", "wpa2", ...). Never NULL.
const char *lxveos_wifi_authmode_str(uint8_t authmode);

#ifdef __cplusplus
}
#endif
