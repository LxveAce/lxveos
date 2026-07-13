// LxveOS Wi-Fi recon (M1) — passive AP scan. See lxveos_wifi.h. The Wi-Fi stack is brought up lazily on
// the first scan so a headless unit that never scans pays neither the ~50 KB heap nor the boot time.
// PASSIVE scan only: the radio listens for beacons and transmits nothing.
#include "lxveos_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "lxveos_wifi";

// Per-channel passive dwell (ms). ~120 ms comfortably spans the typical 100 ms beacon interval so few APs
// are missed, while keeping a full 2.4 GHz sweep to ~1.5 s.
#define LXVEOS_WIFI_PASSIVE_DWELL_MS 120

static bool s_wifi_up;  // the Wi-Fi stack has been initialised + started (STA)

// Bring the Wi-Fi stack up in station mode, once. NVS is already initialised by app_main. The default
// event loop / netif may in principle be created elsewhere later, so ESP_ERR_INVALID_STATE from those is
// tolerated (already-exists), not fatal.
static esp_err_t ensure_wifi_up(void)
{
    if (s_wifi_up) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(e, TAG, "event loop");
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_wifi_up = true;
    return ESP_OK;
}

esp_err_t lxveos_wifi_scan(lxveos_wifi_ap_t *out, size_t max, size_t *found)
{
    if (found != NULL) {
        *found = 0;
    }
    if (out == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    const wifi_scan_config_t sc = {
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time = {.passive = LXVEOS_WIFI_PASSIVE_DWELL_MS},
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&sc, true), TAG, "scan start");  // block until complete

    uint16_t n = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&n), TAG, "ap num");
    if (n == 0) {
        return ESP_OK;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();  // drop the driver-held results we can't fetch
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = n;
    esp_err_t e = esp_wifi_scan_get_ap_records(&got, recs);
    if (e == ESP_OK) {
        size_t k = 0;
        for (uint16_t i = 0; i < got && k < max; i++) {
            memcpy(out[k].ssid, recs[i].ssid, sizeof(out[k].ssid) - 1);
            out[k].ssid[sizeof(out[k].ssid) - 1] = '\0';
            out[k].rssi = recs[i].rssi;
            out[k].channel = recs[i].primary;
            memcpy(out[k].bssid, recs[i].bssid, sizeof(out[k].bssid));
            out[k].authmode = (uint8_t)recs[i].authmode;
            k++;
        }
        if (found != NULL) {
            *found = k;
        }
    }
    free(recs);
    return e;
}

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
