#pragma once
// Minimal host-test stub for ESP-IDF's esp_wifi_types.h — just the wifi_auth_mode_t enumerators that
// lxveos_wifi_labels.c switches on, with the SAME integer values as ESP-IDF so the host test exercises the
// real authmode -> label / security-grade mapping. NOT used by the firmware build, which pulls the real
// header via the esp_wifi component. If IDF ever renumbers these, the firmware build (real header) is the
// source of truth; keep this stub in step with it.
#include <stdint.h>

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP = 1,
    WIFI_AUTH_WPA_PSK = 2,
    WIFI_AUTH_WPA2_PSK = 3,
    WIFI_AUTH_WPA_WPA2_PSK = 4,
    WIFI_AUTH_WPA2_ENTERPRISE = 5,
    WIFI_AUTH_WPA3_PSK = 6,
    WIFI_AUTH_WPA2_WPA3_PSK = 7,
    WIFI_AUTH_WAPI_PSK = 8,
    WIFI_AUTH_OWE = 9,
    WIFI_AUTH_MAX,
} wifi_auth_mode_t;
