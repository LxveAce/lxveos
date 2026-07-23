#pragma once
// lxveos_wifi_essidmap — a tiny, PURE BSSID->ESSID map shared by the Wi-Fi recon scans (the EAPOL capture and
// the station/AP scans learn AP names from beacons here, and the capture uses the ESSID as the WPA PBKDF2
// salt). Split out of lxveos_wifi.c so the upsert policy is host-tested (tests/host_c/test_wifi_essidmap.c).
// libc-only, no ESP-IDF.
//
// De-cloak upsert: a hidden AP beacons a zero-length SSID, so its entry starts empty; when the real SSID
// later leaks (a probe-response, or a beacon once the AP reveals), a non-empty SSID UPGRADES that empty entry
// instead of being dropped by a first-write-wins guard. That upgrade is what lets a hidden BSSID be revealed
// and its captured handshake become crackable — before this the entry stayed pinned to "" forever.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXVEOS_ESSID_MAX 24  // BSSID->ESSID entries a scan session tracks

typedef struct {
    uint8_t bssid[6];
    char    ssid[33];  // up to a 32-byte SSID + NUL ("" while an AP stays hidden)
    bool    used;
} lxveos_essid_ent_t;

typedef enum {
    LXVEOS_ESSID_NONE = 0,   // no change (already known with a name, invalid len, or the map is full)
    LXVEOS_ESSID_INSERTED,   // a new BSSID entry was added (its SSID may be "" if the AP is hidden)
    LXVEOS_ESSID_REVEALED,   // an existing hidden ("") entry was upgraded to a real SSID — a de-cloak
} lxveos_essid_result_t;

// Learn/refine `bssid`'s ESSID in the `max`-entry map `ents`. `ssid`/`len` is the SSID from a beacon or a
// probe-response (len 0 = hidden). Rules: a known BSSID that already has a real name is never changed or
// downgraded; a known BSSID still empty is UPGRADED by a non-empty SSID (returns REVEALED); an unknown BSSID
// is inserted if a slot is free (returns INSERTED, even for a hidden AP so a later reveal can upgrade it).
// Returns NONE for an invalid len (<0 or >32), a NULL ssid with len > 0, a full map, or a no-op. `ents` must
// be zero-initialised before the first call.
lxveos_essid_result_t lxveos_essid_upsert(lxveos_essid_ent_t *ents, size_t max, const uint8_t bssid[6],
                                          const uint8_t *ssid, int len);

// The NUL-terminated ESSID stored for `bssid`, or "" if the BSSID is unknown or still hidden. Never NULL.
const char *lxveos_essid_lookup(const lxveos_essid_ent_t *ents, size_t max, const uint8_t bssid[6]);

#ifdef __cplusplus
}
#endif
