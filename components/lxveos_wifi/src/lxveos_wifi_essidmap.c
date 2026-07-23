// lxveos_wifi_essidmap — see lxveos_wifi_essidmap.h. PURE BSSID->ESSID map with a de-cloak-aware upsert,
// libc-only, host-tested off-target (tests/host_c/test_wifi_essidmap.c). Split out of lxveos_wifi.c so the
// first-seen-vs-revealed policy is verifiable without hardware.
#include "lxveos_wifi_essidmap.h"

#include <string.h>  // memcmp / memcpy

static bool bssid_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

lxveos_essid_result_t lxveos_essid_upsert(lxveos_essid_ent_t *ents, size_t max, const uint8_t bssid[6],
                                          const uint8_t *ssid, int len)
{
    if (ents == NULL || bssid == NULL || len < 0 || len > 32 || (len > 0 && ssid == NULL)) {
        return LXVEOS_ESSID_NONE;
    }
    // Known BSSID: upgrade an empty (hidden) entry to a real SSID; never change or downgrade a known name.
    for (size_t i = 0; i < max; i++) {
        if (ents[i].used && bssid_eq(ents[i].bssid, bssid)) {
            if (ents[i].ssid[0] == '\0' && len > 0) {
                memcpy(ents[i].ssid, ssid, (size_t)len);
                ents[i].ssid[len] = '\0';
                return LXVEOS_ESSID_REVEALED;
            }
            return LXVEOS_ESSID_NONE;
        }
    }
    // Unknown BSSID: take a free slot (even for a hidden AP, so a later reveal can upgrade it).
    for (size_t i = 0; i < max; i++) {
        if (!ents[i].used) {
            memcpy(ents[i].bssid, bssid, 6);
            if (len > 0) {
                memcpy(ents[i].ssid, ssid, (size_t)len);
            }
            ents[i].ssid[len] = '\0';
            ents[i].used = true;
            return LXVEOS_ESSID_INSERTED;
        }
    }
    return LXVEOS_ESSID_NONE;  // map full
}

const char *lxveos_essid_lookup(const lxveos_essid_ent_t *ents, size_t max, const uint8_t bssid[6])
{
    if (ents == NULL || bssid == NULL) {
        return "";
    }
    for (size_t i = 0; i < max; i++) {
        if (ents[i].used && bssid_eq(ents[i].bssid, bssid)) {
            return ents[i].ssid;
        }
    }
    return "";
}
