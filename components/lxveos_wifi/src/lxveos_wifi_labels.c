// lxveos_wifi_labels — pure Wi-Fi authmode label + security-grade helpers, split out of lxveos_wifi.c so
// they can be host-unit-tested (tests/host_c/test_wifi_labels.c) against a wifi_auth_mode_t enum stub, with
// no esp_wifi driver dependency. The firmware build compiles this against the real esp_wifi_types.h; the
// mapping (open/wep/wpa2/wpa3 labels and the 0..5 posture grade) is identical either way. Extracted verbatim
// — behaviour-preserving refactor, so the security_audit / apaudit output does not change.
#include "lxveos_wifi.h"

#include "esp_wifi_types.h"

#include <stdint.h>
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
