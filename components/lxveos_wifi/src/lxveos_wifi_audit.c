// lxveos_wifi_audit — see lxveos_wifi_audit.h. PURE Wi-Fi scan analytics, libc-only, host-tested off-target
// (tests/host_c/test_wifi_analytics.c). Split out of the CLI's cmd_eviltwin so the twin classification is
// verifiable without hardware.
#include "lxveos_wifi_audit.h"

#include <string.h>  // strcmp

bool lxveos_twin_first_of_essid(const lxveos_ap_view_t *aps, size_t n, size_t idx)
{
    if (aps == NULL || idx >= n) {
        return false;
    }
    const char *ssid = aps[idx].ssid;
    if (ssid == NULL || ssid[0] == '\0') {
        return false;  // hidden AP: no name to group by
    }
    for (size_t k = 0; k < idx; k++) {
        if (aps[k].ssid != NULL && strcmp(aps[k].ssid, ssid) == 0) {
            return false;  // an earlier AP already carried this ESSID
        }
    }
    return true;
}

lxveos_twin_verdict_t lxveos_twin_analyze(const lxveos_ap_view_t *aps, size_t n, const char *essid)
{
    lxveos_twin_verdict_t v = {0, 0, 0, false};
    if (aps == NULL || essid == NULL || essid[0] == '\0') {
        return v;
    }
    for (size_t j = 0; j < n; j++) {
        if (aps[j].ssid != NULL && strcmp(aps[j].ssid, essid) == 0) {
            v.nbssid++;
            if (aps[j].open) {
                v.nopen++;
            } else {
                v.nenc++;
            }
        }
    }
    v.flagged = (v.nbssid >= 2 || (v.nopen > 0 && v.nenc > 0));
    return v;
}
