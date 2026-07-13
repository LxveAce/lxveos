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

// ── EAPOL / PMKID capture ──────────────────────────────────────────────────────────────────────────
// All parsing runs in the promiscuous rx callback (Wi-Fi task) into bounded static tables; the caller's
// task reads them only after promiscuous is disabled, so there is no concurrent access to guard. Nothing
// is transmitted, and no payload is stored beyond the small ESSID map + 16-byte PMKIDs.
#define LXVEOS_ESSID_MAX 24
#define LXVEOS_HS_MAX    12

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    bool used;
} essid_ent_t;

typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];
    uint8_t pmkid[16];
    bool has_pmkid;
    uint8_t msg_mask;  // bit0..3 = M1..M4 seen
    bool used;
} hs_ent_t;

static essid_ent_t s_essid[LXVEOS_ESSID_MAX];
static hs_ent_t s_hs[LXVEOS_HS_MAX];
static volatile lxveos_wifi_eapol_stats_t s_estats;

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static void essid_upsert(const uint8_t *bssid, const uint8_t *ssid, int len)
{
    if (len < 0 || len > 32) {
        return;
    }
    for (int i = 0; i < LXVEOS_ESSID_MAX; i++) {
        if (s_essid[i].used && mac_eq(s_essid[i].bssid, bssid)) {
            return;  // already known
        }
    }
    for (int i = 0; i < LXVEOS_ESSID_MAX; i++) {
        if (!s_essid[i].used) {
            memcpy(s_essid[i].bssid, bssid, 6);
            memcpy(s_essid[i].ssid, ssid, (size_t)len);
            s_essid[i].ssid[len] = '\0';
            s_essid[i].used = true;
            s_estats.essids++;
            return;
        }
    }
}

static hs_ent_t *hs_find_or_add(const uint8_t *ap, const uint8_t *sta)
{
    for (int i = 0; i < LXVEOS_HS_MAX; i++) {
        if (s_hs[i].used && mac_eq(s_hs[i].ap, ap) && mac_eq(s_hs[i].sta, sta)) {
            return &s_hs[i];
        }
    }
    for (int i = 0; i < LXVEOS_HS_MAX; i++) {
        if (!s_hs[i].used) {
            memcpy(s_hs[i].ap, ap, 6);
            memcpy(s_hs[i].sta, sta, 6);
            s_hs[i].used = true;
            return &s_hs[i];
        }
    }
    return NULL;
}

static const char *essid_lookup(const uint8_t *bssid)
{
    for (int i = 0; i < LXVEOS_ESSID_MAX; i++) {
        if (s_essid[i].used && mac_eq(s_essid[i].bssid, bssid)) {
            return s_essid[i].ssid;
        }
    }
    return "";
}

// Parse an M1 EAPOL-Key's key-data for the RSN PMKID KDE (dd <len> 00-0F-AC 04 <16-byte PMKID>).
static void extract_pmkid(const uint8_t *frame, int len, int kf_off, hs_ent_t *h)
{
    if (h == NULL || h->has_pmkid) {
        return;
    }
    const int kd_len_off = kf_off + 93;  // key-data-length field offset within the EAPOL-Key body
    if (kd_len_off + 2 > len) {
        return;
    }
    int kd_off = kd_len_off + 2;
    int kd_len = (frame[kd_len_off] << 8) | frame[kd_len_off + 1];
    int q = 0;
    while (q + 2 <= kd_len && kd_off + q + 2 <= len) {
        uint8_t t = frame[kd_off + q];
        uint8_t l = frame[kd_off + q + 1];
        if (kd_off + q + 2 + l > len) {
            break;
        }
        if (t == 0xDD && l >= 20 &&
            frame[kd_off + q + 2] == 0x00 && frame[kd_off + q + 3] == 0x0F &&
            frame[kd_off + q + 4] == 0xAC && frame[kd_off + q + 5] == 0x04) {
            memcpy(h->pmkid, &frame[kd_off + q + 6], 16);
            h->has_pmkid = true;
            s_estats.pmkids++;
            return;
        }
        q += 2 + l;
    }
}

static void eapol_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt == NULL) {
        return;
    }
    const uint8_t *f = pkt->payload;
    const int len = pkt->rx_ctrl.sig_len;  // 802.11 frame incl. FCS
    if (len < 24) {
        return;
    }
    const uint8_t ftype = (f[0] >> 2) & 0x3;
    const uint8_t fsub = (f[0] >> 4) & 0xF;
    const bool fromds = (f[1] & 0x02) != 0;

    // Management beacon (8) / probe-response (5): learn BSSID(addr3) -> SSID (tag 0 after 12 fixed bytes).
    if (type == WIFI_PKT_MGMT && ftype == 0 && (fsub == 8 || fsub == 5)) {
        s_estats.beacons++;
        const uint8_t *bssid = f + 16;
        int p = 24 + 12;
        while (p + 2 <= len) {
            uint8_t tag = f[p];
            uint8_t tlen = f[p + 1];
            if (p + 2 + tlen > len) {
                break;
            }
            if (tag == 0) {  // SSID element
                essid_upsert(bssid, f + p + 2, tlen);
                break;
            }
            p += 2 + tlen;
        }
        return;
    }

    if (ftype != 2) {  // only data frames carry EAPOL
        return;
    }
    int hdr = 24;
    if (fsub & 0x08) {  // QoS data subtypes carry a 2-byte QoS control
        hdr += 2;
    }
    if (len < hdr + 8 + 4) {
        return;
    }
    const uint8_t *llc = f + hdr;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03)) {
        return;  // not LLC/SNAP
    }
    if (((llc[6] << 8) | llc[7]) != 0x888E) {
        return;  // not EAPOL
    }
    const int eapol_off = hdr + 8;
    if (eapol_off + 4 > len || f[eapol_off + 1] != 3) {
        return;  // not an EAPOL-Key frame
    }
    s_estats.eapol_frames++;
    const int kf_off = eapol_off + 4;  // EAPOL-Key body
    if (kf_off + 3 > len) {
        return;
    }
    const uint16_t key_info = (f[kf_off + 1] << 8) | f[kf_off + 2];
    const bool mic = (key_info & 0x0100) != 0;
    const bool ack = (key_info & 0x0080) != 0;
    const bool install = (key_info & 0x0040) != 0;
    const bool secure = (key_info & 0x0200) != 0;
    int msg = 0;
    if (ack && !mic) {
        msg = 1;
    } else if (mic && !ack && !secure) {
        msg = 2;
    } else if (mic && ack && install) {
        msg = 3;
    } else if (mic && !ack && secure) {
        msg = 4;
    }

    // AP is the BSSID: FromDS => addr2 is the AP, else (ToDS) addr1 is the AP.
    const uint8_t *addr1 = f + 4;
    const uint8_t *addr2 = f + 10;
    const uint8_t *ap = fromds ? addr2 : addr1;
    const uint8_t *sta = fromds ? addr1 : addr2;
    hs_ent_t *h = hs_find_or_add(ap, sta);
    if (h != NULL && msg >= 1 && msg <= 4) {
        h->msg_mask |= (uint8_t)(1u << (msg - 1));
    }
    switch (msg) {
    case 1: s_estats.m1++; extract_pmkid(f, len, kf_off, h); break;
    case 2: s_estats.m2++; break;
    case 3: s_estats.m3++; break;
    case 4: s_estats.m4++; break;
    default: break;
    }
}

esp_err_t lxveos_wifi_eapol_capture(uint32_t seconds, lxveos_wifi_line_cb emit,
                                    lxveos_wifi_eapol_stats_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (seconds == 0) {
        seconds = 15;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    memset((void *)&s_estats, 0, sizeof(s_estats));
    memset(s_essid, 0, sizeof(s_essid));
    memset(s_hs, 0, sizeof(s_hs));

    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filt), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(eapol_rx_cb), TAG, "promisc cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promisc on");

    static const uint8_t chans[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    const int nch = (int)(sizeof(chans) / sizeof(chans[0]));
    const int64_t end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    uint8_t swept = 0;
    int i = 0;
    while (esp_timer_get_time() < end_us) {
        esp_wifi_set_channel(chans[i], WIFI_SECOND_CHAN_NONE);
        if (swept < 255) {
            swept++;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        i = (i + 1) % nch;
    }
    esp_wifi_set_promiscuous(false);

    // Emit a hashcat-22000 WPA*01 line for every captured PMKID (runs here, on the caller's task).
    if (emit != NULL) {
        for (int j = 0; j < LXVEOS_HS_MAX; j++) {
            if (!s_hs[j].used || !s_hs[j].has_pmkid) {
                continue;
            }
            char line[160];
            int n = 0;
            n += snprintf(line + n, sizeof(line) - n, "WPA*01*");
            for (int k = 0; k < 16; k++) {
                n += snprintf(line + n, sizeof(line) - n, "%02x", s_hs[j].pmkid[k]);
            }
            n += snprintf(line + n, sizeof(line) - n, "*%02x%02x%02x%02x%02x%02x*",
                          s_hs[j].ap[0], s_hs[j].ap[1], s_hs[j].ap[2],
                          s_hs[j].ap[3], s_hs[j].ap[4], s_hs[j].ap[5]);
            n += snprintf(line + n, sizeof(line) - n, "%02x%02x%02x%02x%02x%02x*",
                          s_hs[j].sta[0], s_hs[j].sta[1], s_hs[j].sta[2],
                          s_hs[j].sta[3], s_hs[j].sta[4], s_hs[j].sta[5]);
            const char *essid = essid_lookup(s_hs[j].ap);
            for (const char *e = essid; *e && n < (int)sizeof(line) - 6; e++) {
                n += snprintf(line + n, sizeof(line) - n, "%02x", (uint8_t)*e);
            }
            snprintf(line + n, sizeof(line) - n, "***");
            emit(line);
        }
    }

    if (out != NULL) {
        memcpy(out, (const void *)&s_estats, sizeof(*out));
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
