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

// Run a PASSIVE packet monitor for ~`seconds` (0 -> a default) in promiscuous mode. `channel` 0 hops the
// whole 2.4 GHz plan; 1-13 LOCKS to that one channel for the entire window (concentrate on a known AP's
// channel). Listens only — enables promiscuous RX, transmits NOTHING (no probe/deauth/beacon). Brings the
// Wi-Fi stack up on first use. Blocks for the duration, then disables promiscuous and writes the tally to
// *out. Returns ESP_OK or an esp_err_t (out zeroed on failure). Captures no payloads/PII — only counts.
esp_err_t lxveos_wifi_sniff(uint32_t seconds, uint8_t channel, lxveos_wifi_sniff_stats_t *out);

// Tally from one EAPOL/PMKID capture session.
typedef struct {
    uint32_t beacons;       // beacons/probe-responses parsed (source of the BSSID->ESSID map)
    uint32_t essids;        // distinct BSSID->ESSID entries learned
    uint32_t eapol_frames;  // EAPOL-Key frames seen
    uint32_t m1, m2, m3, m4;  // 4-way-handshake messages seen (by key-info classification)
    uint32_t pmkids;        // RSN PMKIDs extracted from M1 (each yields a hashcat WPA*01 line)
    uint32_t mics;          // M1+M2 pairs with a matching replay counter (each yields a hashcat WPA*02 line)
    uint8_t channels_swept;
} lxveos_wifi_eapol_stats_t;

// Sink for one text line the capture wants to surface (a hashcat-22000 WPA*01 PMKID or WPA*02 EAPOL/MIC
// line). UI-free: the caller supplies the printer, so the driver stays free of stdio. Called from the
// caller's task (after the promiscuous session ends), never from the Wi-Fi task.
typedef void (*lxveos_wifi_line_cb)(const char *line);

// PASSIVE EAPOL/PMKID capture for ~`seconds`. `channel` 0 hops the 2.4 GHz plan; 1-13 LOCKS to that channel
// for the whole window (dwell on a known AP's channel for better handshake/PMKID odds). Parses beacons into
// a BSSID->ESSID map, detects EAPOL-Key handshake messages (M1-M4), extracts any RSN PMKID from an M1, and
// pairs the M2 (MIC + EAPOL frame) with an ANONCE source by replay counter. It emits a ready-to-crack
// hashcat-22000 line per artifact via `emit` (may be NULL): `WPA*01*...` for a PMKID and
// `WPA*02*<mic>*<ap>*<sta>*<essid>*<anonce>*<eapol>*<mp>` for a captured handshake — it pairs an ANONCE
// source (M1/M3) with an EAPOL+MIC source (M2/M4) by replay counter, emitting one of three hcxtools message
// pairs: M1+M2 -> mp 00 (M12E2), M3+M2 -> mp 02 (M32E2, when M1 was missed), or M3+M4 -> mp 05 (M34E4, when
// neither M1 nor M2 was seen). EAPOL bytes carry the MIC zeroed; M2-based pairs are preferred and a zeroed-
// nonce M4 is dropped as unusable. LISTEN ONLY — transmits nothing and NEVER deauthenticates to force a
// handshake; it captures only what is already in the air. Stats -> *out. Returns ESP_OK/esp_err_t.
esp_err_t lxveos_wifi_eapol_capture(uint32_t seconds, uint8_t channel, lxveos_wifi_line_cb emit,
                                    lxveos_wifi_eapol_stats_t *out);

// One discovered client station and the AP it is talking to (learned passively from data frames).
typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];    // the client MAC (never a broadcast/multicast address)
    char essid[33];    // the AP's ESSID if a beacon was also seen this session, else ""
    uint32_t frames;   // data frames observed between this client and AP
    int8_t rssi;       // strongest RSSI seen for the pair (dBm)
} lxveos_wifi_client_t;

// PASSIVE client-station scan for ~`seconds`. `channel` 0 hops the whole 2.4 GHz plan; 1-13 LOCKS to that
// one channel for the entire window (concentrate on a known AP's channel). Runs promiscuous and infers
// client<->AP links from the addresses in data frames (ToDS/FromDS), learning AP ESSIDs from beacons along
// the way. Listens only — transmits NOTHING. Copies up to `max` client records into `out`, sets *found;
// also reports the beacon count via *beacons (may be NULL). Returns ESP_OK or an esp_err_t.
esp_err_t lxveos_wifi_sta_scan(uint32_t seconds, uint8_t channel, lxveos_wifi_client_t *out, size_t max,
                               size_t *found, uint32_t *beacons);

// Tally from one deauth/disassoc watch — a passive detector for the classic Wi-Fi deauthentication
// attack. `top_bssid`/`top_count` name the transmitter that sent the most deauth/disassoc frames.
typedef struct {
    uint32_t beacons;
    uint32_t deauth;      // deauthentication frames (mgmt subtype 12)
    uint32_t disassoc;    // disassociation frames (mgmt subtype 10)
    uint8_t top_bssid[6]; // transmitter (addr2) responsible for the most deauth/disassoc frames
    uint32_t top_count;
    uint8_t channels_swept;
} lxveos_wifi_deauth_stats_t;

// PASSIVE deauth/disassoc watch for ~`seconds`. `channel` 0 hops the whole 2.4 GHz plan; 1-13 LOCKS to
// that one channel for the entire window (concentrate on a known AP's channel). Runs promiscuous and
// counts deauthentication / disassociation management frames (the signature of a deauth attack or a rogue
// AP kicking clients), tracking the busiest transmitter. Listens only — transmits NOTHING, and of course
// sends no deauth frames itself. Writes the tally to *out. Returns ESP_OK or an esp_err_t.
esp_err_t lxveos_wifi_deauth_watch(uint32_t seconds, uint8_t channel, lxveos_wifi_deauth_stats_t *out);

// Short human label for a wifi_auth_mode_t value ("open", "wpa2", ...). Never NULL.
const char *lxveos_wifi_authmode_str(uint8_t authmode);

// True if the auth mode is an open (unencrypted) network. Lets callers flag open/encrypted "twins"
// without pulling in esp_wifi's enum.
bool lxveos_wifi_is_open(uint8_t authmode);

#ifdef __cplusplus
}
#endif
