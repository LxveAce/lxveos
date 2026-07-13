// LxveOS Wi-Fi recon (M1) — passive AP scan. See lxveos_wifi.h. The Wi-Fi stack is brought up lazily on
// the first scan so a headless unit that never scans pays neither the ~50 KB heap nor the boot time.
// PASSIVE scan only: the radio listens for beacons and transmits nothing.
#include "lxveos_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
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

// Live tally for an in-flight sniff. Written from the Wi-Fi task's promiscuous callback, read by the
// caller after promiscuous is disabled. Counters are monotonic so the benign read/write overlap during a
// session is harmless; we snapshot only after esp_wifi_set_promiscuous(false) quiesces the callback.
static volatile lxveos_wifi_sniff_stats_t s_sniff;

static void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    s_sniff.total++;
    switch (type) {
    case WIFI_PKT_MGMT: s_sniff.mgmt++; break;
    case WIFI_PKT_CTRL: s_sniff.ctrl++; break;
    case WIFI_PKT_DATA: s_sniff.data++; break;
    default:            s_sniff.misc++; break;
    }
    if (pkt != NULL) {
        int ch = pkt->rx_ctrl.channel;
        if (ch >= 1 && ch <= 13) {
            s_sniff.per_channel[ch]++;
        }
    }
}

esp_err_t lxveos_wifi_sniff(uint32_t seconds, lxveos_wifi_sniff_stats_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (seconds == 0) {
        seconds = 8;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    memset((void *)&s_sniff, 0, sizeof(s_sniff));
    const wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filt), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb), TAG, "promisc cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promisc on");

    // Manual channel hop across the 2.4 GHz plan for the session duration. In promiscuous mode we drive the
    // channel ourselves (no association), dwelling long enough on each to catch a beacon interval or two.
    static const uint8_t chans[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    const int nch = (int)(sizeof(chans) / sizeof(chans[0]));
    const int64_t end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    uint8_t swept = 0;
    int i = 0;
    while (esp_timer_get_time() < end_us) {
        esp_wifi_set_channel(chans[i], WIFI_SECOND_CHAN_NONE);
        if (swept < 255) {
            swept++;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        i = (i + 1) % nch;
    }

    esp_wifi_set_promiscuous(false);
    if (out != NULL) {
        memcpy(out, (const void *)&s_sniff, sizeof(*out));
        out->channels_swept = swept;
    }
    return ESP_OK;
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
