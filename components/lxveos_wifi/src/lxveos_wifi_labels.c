// lxveos_wifi_labels — pure Wi-Fi authmode label + security-grade helpers, split out of lxveos_wifi.c so
// they can be host-unit-tested (tests/host_c/test_wifi_labels.c) against a wifi_auth_mode_t enum stub, with
// no esp_wifi driver dependency. The firmware build compiles this against the real esp_wifi_types.h; the
// mapping (open/wep/wpa2/wpa3 labels and the 0..5 posture grade) is identical either way. Extracted verbatim
// — behaviour-preserving refactor, so the security_audit / apaudit output does not change.
#include "lxveos_wifi.h"

#include "esp_wifi_types.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *lxveos_wifi_authmode_str(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:            return "wep";
    case WIFI_AUTH_WPA_PSK:        return "wpa";
    case WIFI_AUTH_WPA2_PSK:       return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:   return "wpa/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK:       return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:  return "wpa2/3";
    default:                       return "?";
    }
}

bool lxveos_wifi_is_open(uint8_t authmode)
{
    return (wifi_auth_mode_t)authmode == WIFI_AUTH_OPEN;
}

bool lxveos_mac_is_random(uint8_t first_octet)
{
    return (first_octet & 0x02u) != 0u;
}

// ── OUI -> vendor enrichment (pure core, #30) ────────────────────────────────────────────────────────────
// A small curated subset of the IEEE MA-L (24-bit) OUI registry — the chip and device vendors a BLE/Wi-Fi recon
// scan is most likely to see (Espressif is the ESP32 itself; TI/Nordic make the common BLE radios). Each entry
// is VERIFIED against the authoritative IEEE registry via the Wireshark `manuf` file (the same source as
// cyber-controller's oui_table, retrieved 2026-07-23) — NOT from memory. Deliberately small (flash budget) and
// certain-only: an unknown OUI, or any locally-administered (randomized) address, resolves to NULL and the caller
// shows the raw hex — a vendor is never guessed. Only globally-unique 24-bit MA-L blocks are carried.
static const struct {
    uint32_t    oui;      // 24-bit OUI packed big-endian: mac[0]<<16 | mac[1]<<8 | mac[2]
    const char *vendor;
} WIFI_OUI_VENDORS[] = {
    {0x004B12u, "Espressif"},   {0x007007u, "Espressif"},   {0x048308u, "Espressif"},
    {0x000393u, "Apple"},       {0x000502u, "Apple"},       {0x000A27u, "Apple"},
    {0x0000F0u, "Samsung"},     {0x0007ABu, "Samsung"},
    {0x001A11u, "Google"},      {0x00F620u, "Google"},      {0x04006Eu, "Google"},
    {0x28CDC1u, "RaspberryPi"}, {0x2CCF67u, "RaspberryPi"},
    {0x0002B3u, "Intel"},       {0x000347u, "Intel"},
    {0x007147u, "Amazon"},      {0x008621u, "Amazon"},      {0x00BB3Au, "Amazon"},
    {0x001237u, "TI"},          {0x00124Bu, "TI"},          {0x0012D1u, "TI"},
    {0xF4CE36u, "Nordic"},
    {0x0003FFu, "Microsoft"},   {0x00125Au, "Microsoft"},
    {0x009EC8u, "Xiaomi"},      {0x00C30Au, "Xiaomi"},      {0x00EC0Au, "Xiaomi"},
    {0x00000Cu, "Cisco"},       {0x000142u, "Cisco"},       {0x000143u, "Cisco"},
    {0x00156Du, "Ubiquiti"},    {0x002722u, "Ubiquiti"},    {0x0418D6u, "Ubiquiti"},
};

const char *lxveos_oui_vendor(const uint8_t *mac)
{
    if (mac == NULL) {
        return NULL;
    }
    // A randomized / locally-administered address (bit 0x02 of the first octet) has no registry OUI — never
    // attribute one to a vendor. No IEEE-assigned OUI has that bit set, so this can't hide a real match.
    if (lxveos_mac_is_random(mac[0])) {
        return NULL;
    }
    uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | (uint32_t)mac[2];
    for (size_t i = 0; i < sizeof(WIFI_OUI_VENDORS) / sizeof(WIFI_OUI_VENDORS[0]); i++) {
        if (WIFI_OUI_VENDORS[i].oui == oui) {
            return WIFI_OUI_VENDORS[i].vendor;
        }
    }
    return NULL;
}

int lxveos_wifi_auth_grade(uint8_t authmode, const char **note)
{
    const char *n;
    int g;
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:            g = 0; n = "OPEN — no encryption, traffic is cleartext"; break;
    case WIFI_AUTH_WEP:             g = 1; n = "WEP — broken cipher, trivially cracked"; break;
    case WIFI_AUTH_WPA_PSK:         g = 2; n = "WPA — deprecated TKIP, upgrade to WPA2/3"; break;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA2_ENTERPRISE: g = 3; n = "WPA2"; break;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:   g = 4; n = "WPA3"; break;
    default:                        g = 5; n = "other"; break;
    }
    if (note != NULL) {
        *note = n;
    }
    return g;
}

// ── EAPOL 4-way-handshake message classification — pure core ─────────────────────────────────────────────
// Classify an 802.11 EAPOL-Key key-info field into a 4-way-handshake message number (1..4), or 0 if the frame
// is not one of the four PAIRWISE handshake messages. The pairwise Key-Type bit (0x0008) is required first: a
// GROUP-key rekey handshake carries it clear, and without this gate a group-rekey message (MIC set, ACK clear,
// Secure clear) false-matches M4 and inflates the m4/handshake stats. lxveos_wifi.c's EAPOL capture calls this.
uint8_t lxveos_wifi_eapol_msg(uint16_t key_info)
{
    if ((key_info & 0x0008u) == 0u) {   // Key Type = group (not a pairwise/PTK 4-way message)
        return 0;
    }
    const bool mic     = (key_info & 0x0100u) != 0u;
    const bool ack     = (key_info & 0x0080u) != 0u;
    const bool install = (key_info & 0x0040u) != 0u;
    const bool secure  = (key_info & 0x0200u) != 0u;
    if (ack && !mic) {
        return 1;
    }
    if (mic && !ack && !secure) {
        return 2;
    }
    if (mic && ack && install) {
        return 3;
    }
    if (mic && !ack && secure) {
        return 4;
    }
    return 0;
}

// ── Pwnagotchi presence detection — pure core (ported from ESP32 Marauder "Detect Pwnagotchi", MIT) ──────
// A Pwnagotchi beacons from the fixed grid source MAC de:ad:be:ef:de:ad, stuffing a JSON identity object into
// the beacon's (oversized) SSID element. These helpers are the host-tested pure core: the MAC match and a
// small, allocation-free extractor for the name + total-handshakes count. No radio here — lxveos_wifi.c runs
// the passive beacon watch that hands frames to them.

static const uint8_t PWNAGOTCHI_MAC[6] = {0xde, 0xad, 0xbe, 0xef, 0xde, 0xad};

bool lxveos_wifi_is_pwnagotchi_mac(const uint8_t *mac)
{
    return mac != NULL && memcmp(mac, PWNAGOTCHI_MAC, sizeof(PWNAGOTCHI_MAC)) == 0;
}

// Find the first occurrence of NUL-terminated `key` in the `len`-bounded (not necessarily NUL-terminated)
// buffer `hay`. Returns the index just past the match, or -1 if not found.
static int find_key(const char *hay, size_t len, const char *key)
{
    size_t klen = strlen(key);
    if (klen == 0 || klen > len) {
        return -1;
    }
    for (size_t i = 0; i + klen <= len; i++) {
        if (memcmp(hay + i, key, klen) == 0) {
            return (int)(i + klen);
        }
    }
    return -1;
}

// Advance `i` (bounded by `len`) past the next ':' and any following spaces/tabs. Leaves `i` at the value.
static size_t skip_to_value(const char *s, size_t len, size_t i)
{
    while (i < len && s[i] != ':') {
        i++;
    }
    if (i < len) {
        i++;   // step past ':'
    }
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    return i;
}

// NOTE: on-device callers MUST gate on lxveos_wifi_is_pwnagotchi_mac() first — this parser trusts that the
// buffer is a Pwnagotchi advertisement and will happily read the JSON keys out of any SSID that contains them.
bool lxveos_wifi_pwnagotchi_parse(const char *essid, size_t essid_len, char *name, size_t name_cap,
                                  uint32_t *pwnd_tot)
{
    if (name != NULL && name_cap > 0) {
        name[0] = '\0';
    }
    if (pwnd_tot != NULL) {
        *pwnd_tot = 0;
    }
    if (essid == NULL || essid_len == 0) {
        return false;
    }
    // Only treat it as a Pwnagotchi identity object if it carries one of the expected JSON keys — a plain SSID
    // must never be mis-read as a Pwnagotchi.
    if (find_key(essid, essid_len, "\"name\"") < 0 && find_key(essid, essid_len, "\"pwnd_tot\"") < 0) {
        return false;
    }
    bool got = false;

    int p = find_key(essid, essid_len, "\"name\"");
    if (p >= 0 && name != NULL && name_cap > 0) {
        size_t i = skip_to_value(essid, essid_len, (size_t)p);
        if (i < essid_len && essid[i] == '"') {
            i++;   // past opening quote
            size_t o = 0;
            while (i < essid_len && essid[i] != '"' && o + 1 < name_cap) {
                name[o++] = essid[i++];
            }
            name[o] = '\0';
            got = true;
        }
    }

    p = find_key(essid, essid_len, "\"pwnd_tot\"");
    if (p >= 0 && pwnd_tot != NULL) {
        size_t i = skip_to_value(essid, essid_len, (size_t)p);
        uint32_t v = 0;
        bool any = false;
        while (i < essid_len && essid[i] >= '0' && essid[i] <= '9') {
            uint32_t d = (uint32_t)(essid[i] - '0');
            // Saturate instead of wrapping — a spoofed count must not overflow uint32 into a small number.
            v = (v > (UINT32_MAX - d) / 10u) ? UINT32_MAX : v * 10u + d;
            i++;
            any = true;
        }
        if (any) {
            *pwnd_tot = v;
            got = true;
        }
    }
    return got;
}

// ── WiGLE-1.6 wardrive CSV export (pure formatters, #31) ─────────────────────────────────────────────────
// Emit rows a host can upload straight to wigle.net. The format mirrors cyber-controller's wardrive.py (the
// authoritative in-repo WiGLE emitter): the capability-bracket AuthMode, the channel->frequency map, and the
// 14-column row order. SSID quoting is the caller's csv_quote_field step (lxveos always-quotes) so this stays a
// pure layout formatter with no cross-component dependency.

void lxveos_wifi_wigle_caps(uint8_t authmode, char *buf, size_t buflen)
{
    if (buflen == 0) {
        return;
    }
    const char *tok;
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_WEP:             tok = "WEP";  break;
    case WIFI_AUTH_WPA_PSK:         tok = "WPA";  break;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA2_ENTERPRISE: tok = "WPA2"; break;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:   tok = "WPA3"; break;
    default:                        tok = NULL;   break;  // OPEN or unknown -> no encryption bracket
    }
    if (tok == NULL) {
        snprintf(buf, buflen, "[ESS]");
    } else {
        snprintf(buf, buflen, "[%s][ESS]", tok);
    }
}

int lxveos_wifi_channel_to_freq(int channel)
{
    if (channel <= 0) {
        return 0;
    }
    if (channel == 14) {
        return 2484;
    }
    if (channel <= 13) {
        return 2407 + channel * 5;
    }
    return 5000 + channel * 5;  // 5 GHz (ch 32..177)
}

size_t lxveos_wifi_wigle_row(char *buf, size_t buflen, const uint8_t bssid[6], const char *ssid_quoted,
                             uint8_t authmode, int channel, int rssi, const char *first_seen,
                             const char *lat, const char *lon, const char *alt)
{
    if (buf == NULL || buflen == 0) {
        return 0;
    }
    char caps[24];
    lxveos_wifi_wigle_caps(authmode, caps, sizeof(caps));
    // Column order: MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,Lat,Lon,Alt,Accuracy,RCOIs,MfgrId,Type.
    int n = snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%d,%d,%d,%s,%s,%s,0,,,WIFI",
                     bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                     ssid_quoted != NULL ? ssid_quoted : "", caps,
                     first_seen != NULL ? first_seen : "", channel, lxveos_wifi_channel_to_freq(channel), rssi,
                     lat != NULL ? lat : "", lon != NULL ? lon : "", alt != NULL ? alt : "");
    return n < 0 ? 0 : (size_t)n;
}

// ── Airspace-occupancy posture tally (#25) ───────────────────────────────────────────────────────────────
// The pure counts `airspace` reports over one passive AP scan: how many of `aps` are open (unencrypted),
// advertise WPS (a WPS-PIN attack surface), and are hidden (blank SSID). Split verbatim out of cmd_airspace so
// the classification is host-tested and shared; behaviour-identical to the former inline loop. Any out-pointer
// may be NULL. Passive/RX — reads scan records only, transmits nothing.
void lxveos_wifi_airspace_tally(const lxveos_wifi_ap_t *aps, size_t n, unsigned *n_open, unsigned *n_wps,
                                unsigned *n_hidden)
{
    unsigned open = 0, wps = 0, hidden = 0;
    if (aps != NULL) {
        for (size_t i = 0; i < n; i++) {
            if (lxveos_wifi_is_open(aps[i].authmode)) {
                open++;
            }
            if (aps[i].wps) {
                wps++;
            }
            if (aps[i].ssid[0] == '\0') {
                hidden++;
            }
        }
    }
    if (n_open != NULL) {
        *n_open = open;
    }
    if (n_wps != NULL) {
        *n_wps = wps;
    }
    if (n_hidden != NULL) {
        *n_hidden = hidden;
    }
}
