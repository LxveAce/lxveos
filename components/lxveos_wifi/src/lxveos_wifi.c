// LxveOS Wi-Fi recon (M1) — passive AP scan. See lxveos_wifi.h. The Wi-Fi stack is brought up lazily on
// the first scan so a headless unit that never scans pays neither the ~50 KB heap nor the boot time.
// PASSIVE scan only: the radio listens for beacons and transmits nothing.
#include "lxveos_wifi.h"
#include "lxveos_wifi_eapol.h"
#include "lxveos_wifi_essidmap.h"

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
            out[k].wps = recs[i].wps;  // ESP-IDF parses the WPS IE from the beacon into this bit
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

// Steer the radio channel for a promiscuous session. `lock_channel` 1-13 stays on that one channel for the
// whole window; 0 hops the 2.4 GHz plan (non-overlapping 1/6/11 first) dwelling `dwell_ms` per channel. In
// promiscuous mode we drive the channel ourselves (no association). The promiscuous callback keeps tallying
// throughout — this only steers the channel and paces the window. Returns the number of channel dwells.
static uint8_t run_channel_loop(uint32_t seconds, uint8_t lock_channel, uint32_t dwell_ms)
{
    const int64_t end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    if (lock_channel >= 1 && lock_channel <= 13) {
        esp_wifi_set_channel(lock_channel, WIFI_SECOND_CHAN_NONE);
        while (esp_timer_get_time() < end_us) {
            vTaskDelay(pdMS_TO_TICKS(dwell_ms));
        }
        return 1;
    }
    static const uint8_t chans[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    const int nch = (int)(sizeof(chans) / sizeof(chans[0]));
    uint8_t swept = 0;
    int i = 0;
    while (esp_timer_get_time() < end_us) {
        esp_wifi_set_channel(chans[i], WIFI_SECOND_CHAN_NONE);
        if (swept < 255) {
            swept++;
        }
        vTaskDelay(pdMS_TO_TICKS(dwell_ms));
        i = (i + 1) % nch;
    }
    return swept;
}

esp_err_t lxveos_wifi_sniff(uint32_t seconds, uint8_t channel, lxveos_wifi_sniff_stats_t *out)
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

    uint8_t swept = run_channel_loop(seconds, channel, 250);

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
#define LXVEOS_HS_MAX    12  // LXVEOS_ESSID_MAX + the essid entry type live in lxveos_wifi_essidmap.h now

typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];
    uint8_t pmkid[16];
    bool has_pmkid;
    uint8_t msg_mask;  // bit0..3 = M1..M4 seen
    bool used;
    // WPA*02 (EAPOL/MIC) material. A crackable line pairs an ANONCE source (M1 or M3, both AP->STA) with an
    // EAPOL/MIC source (M2 or M4, both STA->AP), by replay counter — the three hcxtools message pairs we can
    // build passively (values verified against hcxtools' source):
    //   · M1 + M2 (replay ==)      -> MESSAGEPAIR 00 (M12E2, "challenge"):  EAPOL from M2, ANONCE from M1
    //   · M3 + M2 (M3 == M2's + 1)  -> MESSAGEPAIR 02 (M32E2, "authorized"): EAPOL from M2, ANONCE from M3
    //   · M3 + M4 (replay ==)       -> MESSAGEPAIR 05 (M34E4):               EAPOL from M4, ANONCE from M3
    // M2-based pairs are preferred (M2 always carries a real SNONCE); an M4 with an all-zero nonce is unusable
    // and dropped at capture (as hcxtools does), so a stored M4 always has a real SNONCE inside its EAPOL.
    uint8_t anonce[32];    // ANONCE from M1
    uint8_t m1_replay[8];
    bool has_anonce;
    uint8_t mic[16];       // MIC from M2
    uint8_t m2_replay[8];
    uint8_t eapol[256];    // M2's EAPOL frame, MIC field zeroed
    uint16_t eapol_len;
    bool has_m2;
    uint8_t m3_anonce[32]; // ANONCE echoed by the AP in M3 (same value as M1's, captured independently)
    uint8_t m3_replay[8];  // M3 replay counter (= M2's + 1 in a valid exchange)
    bool has_m3;
    uint8_t m4_mic[16];    // MIC from M4
    uint8_t m4_replay[8];  // M4 replay counter (== M3's in a valid exchange)
    uint8_t m4_eapol[256]; // M4's EAPOL frame, MIC field zeroed
    uint16_t m4_eapol_len;
    bool has_m4;
} hs_ent_t;

static lxveos_essid_ent_t s_essid[LXVEOS_ESSID_MAX];
static hs_ent_t s_hs[LXVEOS_HS_MAX];
static volatile lxveos_wifi_eapol_stats_t s_estats;

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

// Learn an AP's ESSID from a beacon/probe-response into the shared map (the de-cloak-aware upsert lives in
// lxveos_wifi_essidmap). Count a distinct KNOWN essid: a new non-empty entry, or a hidden entry now revealed.
static void essid_upsert(const uint8_t *bssid, const uint8_t *ssid, int len)
{
    lxveos_essid_result_t r = lxveos_essid_upsert(s_essid, LXVEOS_ESSID_MAX, bssid, ssid, len);
    if (r == LXVEOS_ESSID_REVEALED || (r == LXVEOS_ESSID_INSERTED && len > 0)) {
        s_estats.essids++;
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
    return lxveos_essid_lookup(s_essid, LXVEOS_ESSID_MAX, bssid);
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
            // Counted at emit time (below), not here: a PMKID whose AP never reveals its ESSID yields an
            // uncrackable line that we drop, so `pmkids` must track EMITTED WPA*01 lines, not raw captures.
            return;
        }
        q += 2 + l;
    }
}

// EAPOL-Key body field offsets (relative to kf_off, the byte after the 4-byte 802.1X header):
//   +5 replay counter (8) · +13 key nonce (32) · +77 key MIC (16) · +93 key-data-length (2).
#define EK_REPLAY_OFF 5
#define EK_NONCE_OFF  13
#define EK_MIC_OFF    77

// From M1 (AP->STA): stash the ANONCE (the AP's key nonce) and the replay counter for later pairing.
static void store_m1(hs_ent_t *h, const uint8_t *frame, int len, int kf_off)
{
    if (h == NULL || h->has_anonce) {
        return;
    }
    if (kf_off + EK_NONCE_OFF + 32 > len) {
        return;
    }
    memcpy(h->m1_replay, &frame[kf_off + EK_REPLAY_OFF], 8);
    memcpy(h->anonce, &frame[kf_off + EK_NONCE_OFF], 32);
    h->has_anonce = true;
}

// From M2 (STA->AP): stash the MIC, the replay counter, and the full 802.1X/EAPOL-Key frame with the MIC
// field zeroed (hashcat computes the MIC over the frame with MIC=0). `eapol_off` is the 802.1X version byte.
static void store_m2(hs_ent_t *h, const uint8_t *frame, int len, int eapol_off, int kf_off)
{
    if (h == NULL || h->has_m2) {
        return;
    }
    if (kf_off + EK_MIC_OFF + 16 > len) {
        return;  // MIC field not fully present
    }
    // Full frame length = 4-byte 802.1X header + the declared body length (never trust the 802.11 len,
    // which includes FCS/padding). The MIC sits at offset 81 within this frame (eapol_off+4+77).
    int elen = 4 + ((frame[eapol_off + 2] << 8) | frame[eapol_off + 3]);
    if (elen < 99 || elen > (int)sizeof(h->eapol) || eapol_off + elen > len) {
        return;  // implausible / truncated / too large to store
    }
    memcpy(h->mic, &frame[kf_off + EK_MIC_OFF], 16);
    memcpy(h->m2_replay, &frame[kf_off + EK_REPLAY_OFF], 8);
    memcpy(h->eapol, &frame[eapol_off], (size_t)elen);
    memset(&h->eapol[81], 0, 16);  // zero the MIC inside the stored EAPOL frame
    h->eapol_len = (uint16_t)elen;
    h->has_m2 = true;
}

// From M3 (AP->STA, MIC+ACK+Install): stash the ANONCE the AP echoes and the replay counter, so an M2+M3
// pair can be emitted (MESSAGEPAIR 02) when M1 was never seen. M3's replay counter is M2's incremented by 1.
static void store_m3(hs_ent_t *h, const uint8_t *frame, int len, int kf_off)
{
    if (h == NULL || h->has_m3) {
        return;
    }
    if (kf_off + EK_NONCE_OFF + 32 > len) {
        return;
    }
    memcpy(h->m3_replay, &frame[kf_off + EK_REPLAY_OFF], 8);
    memcpy(h->m3_anonce, &frame[kf_off + EK_NONCE_OFF], 32);
    h->has_m3 = true;
}

// From M4 (STA->AP, MIC+Secure, no ACK/Install): stash the MIC + replay + full EAPOL frame (MIC zeroed) so
// an M3+M4 pair can be emitted (MESSAGEPAIR 05) when neither M1 nor M2 was seen. An M4 whose key nonce is
// all-zero carries no usable SNONCE (not crackable) and is dropped — exactly as hcxtools does.
static void store_m4(hs_ent_t *h, const uint8_t *frame, int len, int eapol_off, int kf_off)
{
    if (h == NULL || h->has_m4) {
        return;
    }
    if (kf_off + EK_MIC_OFF + 16 > len || kf_off + EK_NONCE_OFF + 32 > len) {
        return;
    }
    static const uint8_t zero32[32] = {0};
    if (memcmp(&frame[kf_off + EK_NONCE_OFF], zero32, 32) == 0) {
        return;  // zeroed SNONCE -> unusable for cracking
    }
    int elen = 4 + ((frame[eapol_off + 2] << 8) | frame[eapol_off + 3]);
    if (elen < 99 || elen > (int)sizeof(h->m4_eapol) || eapol_off + elen > len) {
        return;
    }
    memcpy(h->m4_mic, &frame[kf_off + EK_MIC_OFF], 16);
    memcpy(h->m4_replay, &frame[kf_off + EK_REPLAY_OFF], 8);
    memcpy(h->m4_eapol, &frame[eapol_off], (size_t)elen);
    memset(&h->m4_eapol[81], 0, 16);  // zero the MIC inside the stored EAPOL frame
    h->m4_eapol_len = (uint16_t)elen;
    h->has_m4 = true;
}

// Read an 8-byte big-endian EAPOL replay counter as a uint64 (for the M2->M3 "+1" pairing check).
static uint64_t replay_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
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
    // Pairwise-gated 4-way-handshake classification (host-tested pure core) — a group-key rekey is not an M1..M4.
    const int msg = lxveos_wifi_eapol_msg(key_info);

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
    case 1: s_estats.m1++; extract_pmkid(f, len, kf_off, h); store_m1(h, f, len, kf_off); break;
    case 2: s_estats.m2++; store_m2(h, f, len, eapol_off, kf_off); break;
    case 3: s_estats.m3++; store_m3(h, f, len, kf_off); break;
    case 4: s_estats.m4++; store_m4(h, f, len, eapol_off, kf_off); break;
    default: break;
    }
}

esp_err_t lxveos_wifi_eapol_capture(uint32_t seconds, uint8_t channel, lxveos_wifi_line_cb emit,
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

    uint8_t swept = run_channel_loop(seconds, channel, 300);
    esp_wifi_set_promiscuous(false);

    // Emit a hashcat-22000 WPA*01 line for every captured PMKID (runs here, on the caller's task).
    if (emit != NULL) {
        for (int j = 0; j < LXVEOS_HS_MAX; j++) {
            if (!s_hs[j].used || !s_hs[j].has_pmkid) {
                continue;
            }
            // The ESSID is the PBKDF2 salt, so a line with an empty ESSID is uncrackable — skip it (and don't
            // count it). The AP may reveal its SSID later; until it does this PMKID isn't a usable artifact.
            const char *essid = essid_lookup(s_hs[j].ap);
            if (essid[0] == '\0') {
                continue;
            }
            char line[160];
            if (lxveos_hc22000_pmkid(line, sizeof(line), s_hs[j].pmkid, s_hs[j].ap,
                                     s_hs[j].sta, essid) == 0) {
                continue;  // defensive: a 0 return (only on an empty ESSID) means uncrackable
            }
            emit(line);
            s_estats.pmkids++;
        }

        // WPA*02 (EAPOL/MIC): pair an ANONCE source (M1/M3) with an EAPOL+MIC source (M2/M4) by replay
        // counter, preferring M2-based pairs (M2 always carries a real SNONCE):
        //   · M1 + M2 (replay ==)      -> MESSAGEPAIR 00 (M12E2, EAPOL from M2, ANONCE from M1)
        //   · M3 + M2 (M3 == M2's + 1) -> MESSAGEPAIR 02 (M32E2, EAPOL from M2, ANONCE from M3)
        //   · M3 + M4 (replay ==)      -> MESSAGEPAIR 05 (M34E4, EAPOL from M4, ANONCE from M3)
        // At most one line per exchange (they carry the same ANONCE). The line can reach ~712 chars (EAPOL up
        // to 256 B + a 32-char ESSID), so it uses its own static buffer.
        for (int j = 0; j < LXVEOS_HS_MAX; j++) {
            hs_ent_t *h = &s_hs[j];
            if (!h->used) {
                continue;
            }
            const uint8_t *anonce, *mic, *eapol;
            uint16_t eapol_len;
            const char *messagepair;
            if (h->has_m2 && h->has_anonce && memcmp(h->m1_replay, h->m2_replay, 8) == 0) {
                anonce = h->anonce;    mic = h->mic;    eapol = h->eapol;    eapol_len = h->eapol_len;
                messagepair = "00";    // M1+M2
            } else if (h->has_m2 && h->has_m3 &&
                       replay_be64(h->m3_replay) == replay_be64(h->m2_replay) + 1) {
                anonce = h->m3_anonce; mic = h->mic;    eapol = h->eapol;    eapol_len = h->eapol_len;
                messagepair = "02";    // M2+M3 ("authorized")
            } else if (h->has_m3 && h->has_m4 &&
                       memcmp(h->m3_replay, h->m4_replay, 8) == 0) {
                anonce = h->m3_anonce; mic = h->m4_mic; eapol = h->m4_eapol; eapol_len = h->m4_eapol_len;
                messagepair = "05";    // M3+M4
            } else {
                continue;  // no valid ANONCE+EAPOL pairing for this exchange
            }
            // ESSID is the salt — an empty-ESSID exchange is uncrackable, so skip it and don't count it in mics.
            const char *essid = essid_lookup(h->ap);
            if (essid[0] == '\0') {
                continue;
            }
            static char l2[800];
            if (lxveos_hc22000_eapol(l2, sizeof(l2), mic, h->ap, h->sta, essid, anonce,
                                     eapol, eapol_len, messagepair) == 0) {
                continue;  // defensive: uncrackable (empty ESSID)
            }
            emit(l2);
            s_estats.mics++;
        }
    }

    if (out != NULL) {
        memcpy(out, (const void *)&s_estats, sizeof(*out));
        out->channels_swept = swept;
    }
    return ESP_OK;
}

// ── Client-station scan ────────────────────────────────────────────────────────────────────────────
// Passively infers client<->AP links from data-frame addresses. Reuses the beacon->ESSID map above (only
// one capture runs at a time; each session memsets the shared tables first). Written by the rx callback,
// read after promiscuous is disabled.
#define LXVEOS_STA_MAX 48

typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];
    uint32_t frames;
    int8_t rssi;
    bool used;
} sta_ent_t;

static sta_ent_t s_sta[LXVEOS_STA_MAX];
static volatile uint32_t s_sta_beacons;

static void sta_upsert(const uint8_t *ap, const uint8_t *sta, int8_t rssi)
{
    for (int i = 0; i < LXVEOS_STA_MAX; i++) {
        if (s_sta[i].used && mac_eq(s_sta[i].ap, ap) && mac_eq(s_sta[i].sta, sta)) {
            s_sta[i].frames++;
            if (rssi > s_sta[i].rssi) {
                s_sta[i].rssi = rssi;  // keep the strongest (closest to 0)
            }
            return;
        }
    }
    for (int i = 0; i < LXVEOS_STA_MAX; i++) {
        if (!s_sta[i].used) {
            memcpy(s_sta[i].ap, ap, 6);
            memcpy(s_sta[i].sta, sta, 6);
            s_sta[i].frames = 1;
            s_sta[i].rssi = rssi;
            s_sta[i].used = true;
            return;
        }
    }
}

static void sta_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt == NULL) {
        return;
    }
    const uint8_t *f = pkt->payload;
    const int len = pkt->rx_ctrl.sig_len;
    if (len < 24) {
        return;
    }
    const uint8_t ftype = (f[0] >> 2) & 0x3;
    const uint8_t fsub = (f[0] >> 4) & 0xF;
    const bool tods = (f[1] & 0x01) != 0;
    const bool fromds = (f[1] & 0x02) != 0;

    if (type == WIFI_PKT_MGMT && ftype == 0 && (fsub == 8 || fsub == 5)) {
        s_sta_beacons++;
        const uint8_t *bssid = f + 16;
        int p = 24 + 12;
        while (p + 2 <= len) {
            uint8_t tag = f[p];
            uint8_t tlen = f[p + 1];
            if (p + 2 + tlen > len) {
                break;
            }
            if (tag == 0) {
                essid_upsert(bssid, f + p + 2, tlen);
                break;
            }
            p += 2 + tlen;
        }
        return;
    }
    if (ftype != 2) {  // only data frames link a client to an AP
        return;
    }
    // Exactly one of ToDS/FromDS distinguishes the AP from the client; skip ad-hoc/WDS (neither/both).
    const uint8_t *addr1 = f + 4;
    const uint8_t *addr2 = f + 10;
    const uint8_t *ap;
    const uint8_t *sta;
    if (tods && !fromds) {
        ap = addr1;
        sta = addr2;
    } else if (fromds && !tods) {
        ap = addr2;
        sta = addr1;
    } else {
        return;
    }
    if (sta[0] & 0x01) {
        return;  // client field is a broadcast/multicast address — not a real station
    }
    sta_upsert(ap, sta, pkt->rx_ctrl.rssi);
}

esp_err_t lxveos_wifi_sta_scan(uint32_t seconds, uint8_t channel, lxveos_wifi_client_t *out, size_t max,
                               size_t *found, uint32_t *beacons)
{
    if (found != NULL) {
        *found = 0;
    }
    if (beacons != NULL) {
        *beacons = 0;
    }
    if (out == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (seconds == 0) {
        seconds = 12;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    memset(s_sta, 0, sizeof(s_sta));
    memset(s_essid, 0, sizeof(s_essid));
    s_sta_beacons = 0;

    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filt), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(sta_rx_cb), TAG, "promisc cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promisc on");

    run_channel_loop(seconds, channel, 300);
    esp_wifi_set_promiscuous(false);

    size_t k = 0;
    for (int j = 0; j < LXVEOS_STA_MAX && k < max; j++) {
        if (!s_sta[j].used) {
            continue;
        }
        memcpy(out[k].ap, s_sta[j].ap, 6);
        memcpy(out[k].sta, s_sta[j].sta, 6);
        out[k].frames = s_sta[j].frames;
        out[k].rssi = s_sta[j].rssi;
        const char *essid = essid_lookup(s_sta[j].ap);
        strncpy(out[k].essid, essid, sizeof(out[k].essid) - 1);
        out[k].essid[sizeof(out[k].essid) - 1] = '\0';
        k++;
    }
    if (found != NULL) {
        *found = k;
    }
    if (beacons != NULL) {
        *beacons = s_sta_beacons;
    }
    return ESP_OK;
}

// ── Probe-request logger (passive recon) ─────────────────────────────────────────────────────────────
// Records the SSIDs nearby client devices are actively looking for. A DIRECTED probe request carries the
// SSID of a network the device has connected to before, so a passive listen reveals a device's saved-
// network history — a classic recon signal and a privacy leak. Written by the rx callback, read after
// promiscuous is disabled. Purely observational: never sends a probe response.
#define LXVEOS_PROBE_MAX 48

typedef struct {
    char ssid[33];
    uint32_t count;
    int8_t rssi;
    bool used;
} probe_ent_t;

static probe_ent_t s_probes[LXVEOS_PROBE_MAX];
static volatile uint32_t s_probe_total;     // every probe request seen (directed + wildcard)
static volatile uint32_t s_probe_wildcard;  // broadcast probes with an empty SSID (no saved-net leak)

static void probe_upsert(const char *ssid, int8_t rssi)
{
    for (int i = 0; i < LXVEOS_PROBE_MAX; i++) {
        if (s_probes[i].used && strcmp(s_probes[i].ssid, ssid) == 0) {
            s_probes[i].count++;
            if (rssi > s_probes[i].rssi) {
                s_probes[i].rssi = rssi;
            }
            return;
        }
    }
    for (int i = 0; i < LXVEOS_PROBE_MAX; i++) {
        if (!s_probes[i].used) {
            size_t n = strnlen(ssid, sizeof(s_probes[i].ssid) - 1);
            memcpy(s_probes[i].ssid, ssid, n);
            s_probes[i].ssid[n] = '\0';
            s_probes[i].count = 1;
            s_probes[i].rssi = rssi;
            s_probes[i].used = true;
            return;
        }
    }
}

static void probe_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt == NULL) {
        return;
    }
    const uint8_t *f = pkt->payload;
    const int len = pkt->rx_ctrl.sig_len;
    if (len < 24) {
        return;
    }
    const uint8_t ftype = (f[0] >> 2) & 0x3;
    const uint8_t fsub = (f[0] >> 4) & 0xF;
    if (type != WIFI_PKT_MGMT || ftype != 0 || fsub != 4) {
        return;  // only probe-request management frames (subtype 4)
    }
    s_probe_total++;
    // Probe requests have NO fixed body fields — the tagged parameters begin at offset 24, and the SSID
    // element (tag 0) is mandated first.
    const int p = 24;
    if (p + 2 > len) {
        return;
    }
    if (f[p] != 0) {
        return;  // first element isn't the SSID — malformed; ignore
    }
    int slen = f[p + 1];
    if (p + 2 + slen > len) {
        return;  // truncated
    }
    if (slen == 0) {
        s_probe_wildcard++;  // wildcard/broadcast probe — reveals no saved network
        return;
    }
    if (slen > 32) {
        slen = 32;
    }
    char name[33];
    memcpy(name, f + p + 2, (size_t)slen);
    name[slen] = '\0';
    probe_upsert(name, pkt->rx_ctrl.rssi);
}

esp_err_t lxveos_wifi_probe_scan(uint32_t seconds, uint8_t channel, lxveos_wifi_probe_t *out, size_t max,
                                 size_t *found, uint32_t *total, uint32_t *wildcard)
{
    if (found != NULL) {
        *found = 0;
    }
    if (total != NULL) {
        *total = 0;
    }
    if (wildcard != NULL) {
        *wildcard = 0;
    }
    if (out == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (seconds == 0) {
        seconds = 12;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    memset(s_probes, 0, sizeof(s_probes));
    s_probe_total = 0;
    s_probe_wildcard = 0;

    const wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filt), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(probe_rx_cb), TAG, "promisc cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promisc on");

    run_channel_loop(seconds, channel, 300);
    esp_wifi_set_promiscuous(false);

    size_t k = 0;
    for (int i = 0; i < LXVEOS_PROBE_MAX && k < max; i++) {
        if (!s_probes[i].used) {
            continue;
        }
        size_t sn = strnlen(s_probes[i].ssid, sizeof(out[k].ssid) - 1);
        memcpy(out[k].ssid, s_probes[i].ssid, sn);
        out[k].ssid[sn] = '\0';
        out[k].count = s_probes[i].count;
        out[k].rssi = s_probes[i].rssi;
        k++;
    }
    if (found != NULL) {
        *found = k;
    }
    if (total != NULL) {
        *total = s_probe_total;
    }
    if (wildcard != NULL) {
        *wildcard = s_probe_wildcard;
    }
    return ESP_OK;
}

// ── Deauth / disassoc watch (passive defense) ────────────────────────────────────────────────────────
// Counts deauthentication (mgmt subtype 12) and disassociation (subtype 10) frames — the fingerprint of a
// deauth attack — and tracks the busiest transmitter. Reuses the promiscuous plumbing; written by the rx
// callback, read after promiscuous is disabled. Purely observational: transmits nothing.
#define LXVEOS_OFFENDER_MAX 16

typedef struct {
    uint8_t bssid[6];
    uint32_t count;
    bool used;
} offender_ent_t;

static offender_ent_t s_offender[LXVEOS_OFFENDER_MAX];
static volatile lxveos_wifi_deauth_stats_t s_dstats;

static void offender_bump(const uint8_t *bssid)
{
    for (int i = 0; i < LXVEOS_OFFENDER_MAX; i++) {
        if (s_offender[i].used && mac_eq(s_offender[i].bssid, bssid)) {
            s_offender[i].count++;
            return;
        }
    }
    for (int i = 0; i < LXVEOS_OFFENDER_MAX; i++) {
        if (!s_offender[i].used) {
            memcpy(s_offender[i].bssid, bssid, 6);
            s_offender[i].count = 1;
            s_offender[i].used = true;
            return;
        }
    }
}

static void deauth_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt == NULL) {
        return;
    }
    const uint8_t *f = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24) {
        return;
    }
    if (type != WIFI_PKT_MGMT || ((f[0] >> 2) & 0x3) != 0) {
        return;
    }
    const uint8_t fsub = (f[0] >> 4) & 0xF;
    if (fsub == 8 || fsub == 5) {
        s_dstats.beacons++;
        return;
    }
    if (fsub == 12) {
        s_dstats.deauth++;
        offender_bump(f + 10);
    } else if (fsub == 10) {
        s_dstats.disassoc++;
        offender_bump(f + 10);
    }
}

esp_err_t lxveos_wifi_deauth_watch(uint32_t seconds, uint8_t channel, lxveos_wifi_deauth_stats_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (seconds == 0) {
        seconds = 15;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    memset((void *)&s_dstats, 0, sizeof(s_dstats));
    memset(s_offender, 0, sizeof(s_offender));

    const wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filt), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(deauth_rx_cb), TAG, "promisc cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promisc on");

    uint8_t swept = run_channel_loop(seconds, channel, 300);
    esp_wifi_set_promiscuous(false);

    if (out != NULL) {
        memcpy(out, (const void *)&s_dstats, sizeof(*out));
        out->channels_swept = swept;
        uint32_t best = 0;
        for (int j = 0; j < LXVEOS_OFFENDER_MAX; j++) {
            if (s_offender[j].used && s_offender[j].count > best) {
                best = s_offender[j].count;
                memcpy(out->top_bssid, s_offender[j].bssid, 6);
                out->top_count = s_offender[j].count;
            }
        }
    }
    return ESP_OK;
}

// ── Pwnagotchi presence watch (passive) ─────────────────────────────────────────────────────────────────
// Flags Wi-Fi beacons sent from the fixed Pwnagotchi grid MAC de:ad:be:ef:de:ad and decodes the JSON identity
// stuffed into the beacon SSID (pure core in lxveos_wifi_labels.c; ported from ESP32 Marauder "Detect
// Pwnagotchi", MIT — see CREDITS.md). LISTEN ONLY — transmits nothing. The scalar counters are volatile
// (written from the promiscuous callback, read after it quiesces); the name buffer is a plain static
// snapshotted after promiscuous is disabled — the same split the deauth watch uses for its offender table.
static volatile uint32_t s_pwn_beacons;
static volatile uint32_t s_pwn_count;
static volatile uint32_t s_pwn_last_tot;
static volatile int8_t   s_pwn_last_rssi;
static volatile bool     s_pwn_found;
static char              s_pwn_name[32];

static void pwnagotchi_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt == NULL) {
        return;
    }
    const uint8_t *f = pkt->payload;
    const int len = pkt->rx_ctrl.sig_len;
    if (len < 24) {
        return;
    }
    // Management beacon only (frame type 0, subtype 8).
    if (type != WIFI_PKT_MGMT || ((f[0] >> 2) & 0x3) != 0 || ((f[0] >> 4) & 0xF) != 8) {
        return;
    }
    s_pwn_beacons++;
    if (!lxveos_wifi_is_pwnagotchi_mac(f + 10)) {   // addr2 = source address
        return;
    }
    s_pwn_count++;
    // Walk the tagged parameters to the SSID element (tag 0) — it carries the JSON identity.
    int p = 24 + 12;
    while (p + 2 <= len) {
        uint8_t tag = f[p];
        uint8_t tlen = f[p + 1];
        if (p + 2 + tlen > len) {
            break;
        }
        if (tag == 0) {
            char name[32] = {0};   // zero-init: parse NUL-terminates, but don't drag stack tail into s_pwn_name
            uint32_t tot = 0;
            if (lxveos_wifi_pwnagotchi_parse((const char *)(f + p + 2), tlen, name, sizeof(name), &tot)) {
                memcpy(s_pwn_name, name, sizeof(name));
                s_pwn_last_tot = tot;
                s_pwn_last_rssi = (int8_t)pkt->rx_ctrl.rssi;
                s_pwn_found = true;
            }
            break;
        }
        p += 2 + tlen;
    }
}

esp_err_t lxveos_wifi_pwnagotchi_watch(uint32_t seconds, uint8_t channel, lxveos_wifi_pwnagotchi_stats_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (seconds == 0) {
        seconds = 15;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_up(), TAG, "wifi bring-up");

    s_pwn_beacons = 0;
    s_pwn_count = 0;
    s_pwn_last_tot = 0;
    s_pwn_last_rssi = 0;
    s_pwn_found = false;
    memset(s_pwn_name, 0, sizeof(s_pwn_name));

    const wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filt), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(pwnagotchi_rx_cb), TAG, "promisc cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "promisc on");

    uint8_t swept = run_channel_loop(seconds, channel, 300);
    esp_wifi_set_promiscuous(false);

    if (out != NULL) {
        out->seconds = seconds;
        out->beacons = s_pwn_beacons;
        out->pwnagotchi = s_pwn_count;
        out->found = s_pwn_found;
        out->last_pwnd_tot = s_pwn_last_tot;
        out->last_rssi = s_pwn_last_rssi;
        out->channels_swept = swept;
        memcpy(out->last_name, s_pwn_name, sizeof(out->last_name));
    }
    return ESP_OK;
}

// lxveos_wifi_authmode_str / lxveos_wifi_is_open / lxveos_wifi_auth_grade live in lxveos_wifi_labels.c
// (pure authmode->label + security-grade math, host-tested off-target in tests/host_c/test_wifi_labels.c).
