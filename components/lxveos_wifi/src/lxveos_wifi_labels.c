// lxveos_wifi_labels — pure Wi-Fi authmode label + security-grade helpers, split out of lxveos_wifi.c so
// they can be host-unit-tested (tests/host_c/test_wifi_labels.c) against a wifi_auth_mode_t enum stub, with
// no esp_wifi driver dependency. The firmware build compiles this against the real esp_wifi_types.h; the
// mapping (open/wep/wpa2/wpa3 labels and the 0..5 posture grade) is identical either way. Extracted verbatim
// — behaviour-preserving refactor, so the security_audit / apaudit output does not change.
#include "lxveos_wifi.h"

#include "esp_wifi_types.h"

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
