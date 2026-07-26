// Host-side unit test for lxveos_wifi_labels (Wi-Fi authmode label + security-grade helpers). Pure libc +
// a wifi_auth_mode_t enum stub (stubs/esp_wifi_types.h), no ESP-IDF toolchain. Built + run by
// tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_wifi.h"

#include "esp_wifi_types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_authmode_str(void)
{
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_OPEN), "open") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WEP), "wep") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WPA_PSK), "wpa") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WPA2_PSK), "wpa2") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WPA_WPA2_PSK), "wpa/2") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WPA2_ENTERPRISE), "wpa2-ent") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WPA3_PSK), "wpa3") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WPA2_WPA3_PSK), "wpa2/3") == 0);
    // Anything outside the known set is the honest "?" (never a mis-label).
    assert(strcmp(lxveos_wifi_authmode_str(WIFI_AUTH_WAPI_PSK), "?") == 0);
    assert(strcmp(lxveos_wifi_authmode_str(200), "?") == 0);
}

static void test_is_open(void)
{
    assert(lxveos_wifi_is_open(WIFI_AUTH_OPEN));
    assert(!lxveos_wifi_is_open(WIFI_AUTH_WEP));
    assert(!lxveos_wifi_is_open(WIFI_AUTH_WPA2_PSK));
    assert(!lxveos_wifi_is_open(WIFI_AUTH_WPA3_PSK));
    assert(!lxveos_wifi_is_open(200));
}

static void test_auth_grade(void)
{
    const char *note = NULL;
    // Weakest -> strongest, with the exact grade the security audit surfaces.
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_OPEN, &note) == 0 && strstr(note, "OPEN") != NULL);
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WEP, &note) == 1 && strstr(note, "WEP") != NULL);
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WPA_PSK, &note) == 2 && strstr(note, "WPA") != NULL);
    // WPA2 family collapses to grade 3, note "WPA2".
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WPA2_PSK, &note) == 3 && strcmp(note, "WPA2") == 0);
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WPA_WPA2_PSK, &note) == 3 && strcmp(note, "WPA2") == 0);
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WPA2_ENTERPRISE, &note) == 3 && strcmp(note, "WPA2") == 0);
    // WPA3 family -> grade 4, note "WPA3".
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WPA3_PSK, &note) == 4 && strcmp(note, "WPA3") == 0);
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WPA2_WPA3_PSK, &note) == 4 && strcmp(note, "WPA3") == 0);
    // Unknown modes -> grade 5 "other" (not mis-graded as secure).
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_WAPI_PSK, &note) == 5 && strcmp(note, "other") == 0);
    assert(lxveos_wifi_auth_grade(200, &note) == 5 && strcmp(note, "other") == 0);
    // note == NULL must be safe (the caller may only want the grade).
    assert(lxveos_wifi_auth_grade(WIFI_AUTH_OPEN, NULL) == 0);

    // Consistency: OPEN is the only mode that is both "open" and grade 0.
    for (int m = 0; m < WIFI_AUTH_MAX; m++) {
        int is_open = lxveos_wifi_is_open((uint8_t)m);
        int grade = lxveos_wifi_auth_grade((uint8_t)m, NULL);
        assert(is_open == (grade == 0));
    }
}

static void test_eapol_msg(void)
{
    // Pairwise 4-way messages (Key Type bit 0x0008 set), by the standard key-info flags.
    assert(lxveos_wifi_eapol_msg(0x0088) == 1);  // pairwise + ACK, no MIC          -> M1
    assert(lxveos_wifi_eapol_msg(0x0108) == 2);  // pairwise + MIC, no ACK/Secure   -> M2
    assert(lxveos_wifi_eapol_msg(0x01C8) == 3);  // pairwise + MIC + ACK + Install  -> M3
    assert(lxveos_wifi_eapol_msg(0x0308) == 4);  // pairwise + MIC + Secure, no ACK -> M4

    // The bug this guards: a GROUP-key rekey frame (pairwise bit CLEAR) that satisfies an M-predicate must NOT
    // be classified as a handshake message. Without the gate, MIC+Secure(+!ACK) would false-match M4, and a
    // bare ACK would false-match M1 — inflating the m4/m1 handshake stats with non-handshake frames.
    assert(lxveos_wifi_eapol_msg(0x0300) == 0);  // MIC + Secure, no pairwise bit -> not a 4-way msg
    assert(lxveos_wifi_eapol_msg(0x0080) == 0);  // bare ACK, no pairwise         -> not M1

    // A pairwise frame that matches no message predicate is 0 (never a false handshake count).
    assert(lxveos_wifi_eapol_msg(0x0208) == 0);  // pairwise + Secure only
    assert(lxveos_wifi_eapol_msg(0x0008) == 0);  // pairwise bit alone
    assert(lxveos_wifi_eapol_msg(0x0000) == 0);
}

static void test_pwnagotchi(void)
{
    // MAC match: only the fixed grid address de:ad:be:ef:de:ad is a Pwnagotchi; anything else isn't.
    const uint8_t pwn[6]  = {0xde, 0xad, 0xbe, 0xef, 0xde, 0xad};
    const uint8_t near[6] = {0xde, 0xad, 0xbe, 0xef, 0xde, 0xae};
    assert(lxveos_wifi_is_pwnagotchi_mac(pwn));
    assert(!lxveos_wifi_is_pwnagotchi_mac(near));
    assert(!lxveos_wifi_is_pwnagotchi_mac(NULL));

    // Full identity object: name + pwnd_tot both extracted. essid need not be NUL-terminated -> pass an
    // explicit length (here strlen, but the parser only trusts the bound).
    char name[32];
    uint32_t tot = 999;
    const char *j = "{\"name\":\"pwny\",\"pwnd_run\":3,\"pwnd_tot\":42,\"uptime\":88}";
    assert(lxveos_wifi_pwnagotchi_parse(j, strlen(j), name, sizeof(name), &tot));
    assert(strcmp(name, "pwny") == 0);
    assert(tot == 42);

    // Missing name, present count: still a plausible parse (count extracted, name cleared).
    const char *j2 = "{\"pwnd_tot\":7}";
    name[0] = 'X';
    tot = 0;
    assert(lxveos_wifi_pwnagotchi_parse(j2, strlen(j2), name, sizeof(name), &tot));
    assert(name[0] == '\0' && tot == 7);

    // A plain SSID (no Pwnagotchi keys) must never be mis-read as one; outputs are cleared.
    tot = 5;
    name[0] = 'X';
    assert(!lxveos_wifi_pwnagotchi_parse("HomeWiFi", 8, name, sizeof(name), &tot));
    assert(name[0] == '\0' && tot == 0);

    // Empty / NULL buffer -> false, cleared outputs, no read.
    assert(!lxveos_wifi_pwnagotchi_parse(NULL, 0, name, sizeof(name), &tot));
    assert(!lxveos_wifi_pwnagotchi_parse("", 0, name, sizeof(name), &tot));

    // Name longer than the buffer is truncated, always NUL-terminated, never overflows.
    char small[5];
    const char *j3 = "{\"name\":\"abcdefgh\"}";
    assert(lxveos_wifi_pwnagotchi_parse(j3, strlen(j3), small, sizeof(small), NULL));
    assert(strcmp(small, "abcd") == 0);   // 4 chars + NUL in a 5-byte buffer

    // Overflow: an absurd pwnd_tot saturates at UINT32_MAX rather than wrapping to a small wrong number.
    uint32_t big = 0;
    const char *jb = "{\"pwnd_tot\":99999999999}";
    assert(lxveos_wifi_pwnagotchi_parse(jb, strlen(jb), NULL, 0, &big));
    assert(big == 4294967295u);

    // Name with no closing quote: copies to the buffer / essid end, truncates cleanly, stays NUL-terminated.
    char nq[8];
    const char *jn = "{\"name\":\"unterminated";
    assert(lxveos_wifi_pwnagotchi_parse(jn, strlen(jn), nq, sizeof(nq), NULL));
    assert(strcmp(nq, "untermi") == 0);   // 7 chars fit in nq[8]
}

static void test_mac_is_random(void)
{
    // the locally-administered bit (0x02) marks a randomized/spoofed MAC
    assert(lxveos_mac_is_random(0x02));
    assert(lxveos_mac_is_random(0x06));
    assert(lxveos_mac_is_random(0xDA));  // e.g. da:a1:19:... a common randomized prefix
    assert(lxveos_mac_is_random(0xFE));
    // a burned-in universally-administered MAC has the bit clear
    assert(!lxveos_mac_is_random(0x00));
    assert(!lxveos_mac_is_random(0x04));
    assert(!lxveos_mac_is_random(0x08));
    assert(!lxveos_mac_is_random(0x24));  // e.g. Espressif 24:0a:c4 (vendor OUI)
    assert(!lxveos_mac_is_random(0xFC));
}

static void test_oui_vendor(void)
{
    // Known vendors resolve from the first three octets; the rest of the MAC is ignored.
    const uint8_t esp[6]    = {0x00, 0x4b, 0x12, 0x11, 0x22, 0x33};
    const uint8_t apple[6]  = {0x00, 0x03, 0x93, 0xaa, 0xbb, 0xcc};
    const uint8_t nordic[6] = {0xf4, 0xce, 0x36, 0x01, 0x02, 0x03};
    const uint8_t ubnt[6]   = {0x04, 0x18, 0xd6, 0x00, 0x00, 0x01};
    assert(strcmp(lxveos_oui_vendor(esp), "Espressif") == 0);
    assert(strcmp(lxveos_oui_vendor(apple), "Apple") == 0);
    assert(strcmp(lxveos_oui_vendor(nordic), "Nordic") == 0);
    assert(strcmp(lxveos_oui_vendor(ubnt), "Ubiquiti") == 0);

    // An unknown but globally-administered OUI -> NULL via the table-miss path (0x10 has the LA bit CLEAR, so
    // the random guard passes through and the lookup loop actually runs and finds nothing).
    const uint8_t unknown[6] = {0x10, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    assert(!lxveos_mac_is_random(unknown[0]));   // sanity: NOT randomized, so the loop is exercised
    assert(lxveos_oui_vendor(unknown) == NULL);

    // A randomized / locally-administered address -> NULL (its OUI is meaningless).
    const uint8_t rnd[6] = {0xda, 0x4b, 0x12, 0x00, 0x00, 0x00};  // 0xda has the LA bit (0x02) set
    assert(lxveos_mac_is_random(rnd[0]));                          // sanity: it IS randomized
    assert(lxveos_oui_vendor(rnd) == NULL);
    // Setting the LA bit on a real OUI's first octet still yields NULL (the random guard fires first).
    const uint8_t esp_la[6] = {0x02, 0x4b, 0x12, 0x00, 0x00, 0x00};  // 0x00 | 0x02 -> locally administered
    assert(lxveos_oui_vendor(esp_la) == NULL);

    // Only the OUI (first 3 octets) matters: two MACs sharing an OUI resolve to the same vendor string.
    const uint8_t esp_a[6] = {0x00, 0x4b, 0x12, 0xde, 0xad, 0xbe};
    const uint8_t esp_b[6] = {0x00, 0x4b, 0x12, 0x00, 0x00, 0x00};
    assert(lxveos_oui_vendor(esp_a) == lxveos_oui_vendor(esp_b));

    // NULL is safe.
    assert(lxveos_oui_vendor(NULL) == NULL);
}

static void test_wigle(void)
{
    char caps[24];
    // AuthMode capability brackets: open/unknown -> [ESS]; else the strongest family + [ESS] (mirrors wardrive.py).
    lxveos_wifi_wigle_caps(WIFI_AUTH_OPEN, caps, sizeof(caps));            assert(strcmp(caps, "[ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WEP, caps, sizeof(caps));             assert(strcmp(caps, "[WEP][ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WPA_PSK, caps, sizeof(caps));         assert(strcmp(caps, "[WPA][ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WPA2_PSK, caps, sizeof(caps));        assert(strcmp(caps, "[WPA2][ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WPA_WPA2_PSK, caps, sizeof(caps));    assert(strcmp(caps, "[WPA2][ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WPA2_ENTERPRISE, caps, sizeof(caps)); assert(strcmp(caps, "[WPA2][ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WPA3_PSK, caps, sizeof(caps));        assert(strcmp(caps, "[WPA3][ESS]") == 0);
    lxveos_wifi_wigle_caps(WIFI_AUTH_WPA2_WPA3_PSK, caps, sizeof(caps));   assert(strcmp(caps, "[WPA3][ESS]") == 0);
    lxveos_wifi_wigle_caps(200, caps, sizeof(caps));                       assert(strcmp(caps, "[ESS]") == 0);  // unknown mode

    // Channel -> centre frequency (2.4 GHz, 5 GHz, and the ch 14 exception).
    assert(lxveos_wifi_channel_to_freq(1) == 2412);
    assert(lxveos_wifi_channel_to_freq(6) == 2437);
    assert(lxveos_wifi_channel_to_freq(11) == 2462);
    assert(lxveos_wifi_channel_to_freq(13) == 2472);
    assert(lxveos_wifi_channel_to_freq(14) == 2484);    // exception: 2484, not 2407+14*5
    assert(lxveos_wifi_channel_to_freq(36) == 5180);
    assert(lxveos_wifi_channel_to_freq(165) == 5825);
    assert(lxveos_wifi_channel_to_freq(0) == 0);
    assert(lxveos_wifi_channel_to_freq(-3) == 0);

    // A full WiGLE-1.6 row (SSID pre-quoted via csv_quote_field; caller-supplied GPS strings).
    char row[256];
    const uint8_t bssid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    size_t n = lxveos_wifi_wigle_row(row, sizeof(row), bssid, "\"MyNet\"", WIFI_AUTH_WPA2_PSK, 6, -50,
                                     "2024-01-01 12:00:00", "37.123456", "-122.654321", "10.5");
    assert(strcmp(row, "AA:BB:CC:DD:EE:FF,\"MyNet\",[WPA2][ESS],2024-01-01 12:00:00,6,2437,-50,"
                       "37.123456,-122.654321,10.5,0,,,WIFI") == 0);
    assert(n == strlen(row));

    // GPS-less (the lxveos default): NULL first_seen + coords -> empty fields; AccuracyMeters stays 0, Type WIFI.
    lxveos_wifi_wigle_row(row, sizeof(row), bssid, "\"MyNet\"", WIFI_AUTH_WPA2_PSK, 6, -50, NULL, NULL, NULL, NULL);
    assert(strcmp(row, "AA:BB:CC:DD:EE:FF,\"MyNet\",[WPA2][ESS],,6,2437,-50,,,,0,,,WIFI") == 0);

    // Open network -> [ESS]; the BSSID prints uppercase hex.
    const uint8_t b2[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    lxveos_wifi_wigle_row(row, sizeof(row), b2, "\"cafe\"", WIFI_AUTH_OPEN, 11, -70, NULL, NULL, NULL, NULL);
    assert(strcmp(row, "00:11:22:33:44:55,\"cafe\",[ESS],,11,2462,-70,,,,0,,,WIFI") == 0);

    // The header constant is the canonical 14-column WiGLE header.
    assert(strcmp(LXVEOS_WIFI_WIGLE_HEADER,
                  "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
                  "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type") == 0);
}

static void test_airspace_tally(void)
{
    // A small scan: [0] open+named, [1] WPA2 with WPS on, [2] hidden (blank SSID) WPA2, [3] plain WPA2.
    lxveos_wifi_ap_t aps[4] = {0};
    snprintf(aps[0].ssid, sizeof(aps[0].ssid), "OpenNet");
    aps[0].authmode = WIFI_AUTH_OPEN;      // -> open
    aps[0].wps = false;
    snprintf(aps[1].ssid, sizeof(aps[1].ssid), "HomeWPS");
    aps[1].authmode = WIFI_AUTH_WPA2_PSK;
    aps[1].wps = true;                     // -> wps
    aps[2].ssid[0] = '\0';                 // -> hidden
    aps[2].authmode = WIFI_AUTH_WPA2_PSK;
    aps[2].wps = false;
    snprintf(aps[3].ssid, sizeof(aps[3].ssid), "Plain");
    aps[3].authmode = WIFI_AUTH_WPA2_PSK;  // counts toward none
    aps[3].wps = false;

    unsigned open = 99, wps = 99, hidden = 99;
    lxveos_wifi_airspace_tally(aps, 4, &open, &wps, &hidden);
    assert(open == 1);
    assert(wps == 1);
    assert(hidden == 1);

    // NULL out-pointers are tolerated (compute + discard, no crash).
    lxveos_wifi_airspace_tally(aps, 4, NULL, NULL, NULL);

    // Empty / NULL input -> all zero.
    open = wps = hidden = 99;
    lxveos_wifi_airspace_tally(NULL, 0, &open, &wps, &hidden);
    assert(open == 0 && wps == 0 && hidden == 0);
    open = wps = hidden = 99;
    lxveos_wifi_airspace_tally(aps, 0, &open, &wps, &hidden);
    assert(open == 0 && wps == 0 && hidden == 0);
}

static void test_apaudit_tally(void)
{
    // A mix across every grade + a WPS AP + a hidden AP: open, WEP, legacy-WPA, WPA2, WPA3, WPA2+WPS,
    // WPA2+hidden, plain WPA2. (grades: open=0, WEP=1, WPA=2, WPA2=3, WPA3=4.)
    lxveos_wifi_ap_t aps[8] = {0};
    aps[0].authmode = WIFI_AUTH_OPEN;     snprintf(aps[0].ssid, sizeof(aps[0].ssid), "a");
    aps[1].authmode = WIFI_AUTH_WEP;      snprintf(aps[1].ssid, sizeof(aps[1].ssid), "b");
    aps[2].authmode = WIFI_AUTH_WPA_PSK;  snprintf(aps[2].ssid, sizeof(aps[2].ssid), "c");
    aps[3].authmode = WIFI_AUTH_WPA2_PSK; snprintf(aps[3].ssid, sizeof(aps[3].ssid), "d");
    aps[4].authmode = WIFI_AUTH_WPA3_PSK; snprintf(aps[4].ssid, sizeof(aps[4].ssid), "e");
    aps[5].authmode = WIFI_AUTH_WPA2_PSK; snprintf(aps[5].ssid, sizeof(aps[5].ssid), "f");
    aps[5].wps = true;                    // WPS on, encrypted -> flagged but not weak
    aps[6].authmode = WIFI_AUTH_WPA2_PSK; aps[6].ssid[0] = '\0';  // hidden, grade 3
    aps[7].authmode = WIFI_AUTH_WPA2_PSK; snprintf(aps[7].ssid, sizeof(aps[7].ssid), "h");  // plain

    lxveos_wifi_apaudit_t t;
    lxveos_wifi_apaudit_tally(aps, 8, &t);
    assert(t.grade_n[0] == 1);   // open
    assert(t.grade_n[1] == 1);   // WEP
    assert(t.grade_n[2] == 1);   // WPA
    assert(t.grade_n[3] == 4);   // WPA2: aps 3, 5, 6, 7
    assert(t.grade_n[4] == 1);   // WPA3
    assert(t.grade_n[5] == 0);   // other
    assert(t.hidden == 1);       // ap6
    assert(t.weak == 3);         // grades 0/1/2
    assert(t.wps == 1);          // ap5
    assert(t.flagged == 4);      // 3 weak + 1 WPS-only (no overlap: the weak APs have no WPS)

    // NULL out -> no crash; NULL/empty aps -> an all-zero tally.
    lxveos_wifi_apaudit_tally(aps, 8, NULL);
    lxveos_wifi_apaudit_tally(NULL, 8, &t);
    assert(t.flagged == 0 && t.weak == 0 && t.wps == 0 && t.hidden == 0 && t.grade_n[3] == 0);
    lxveos_wifi_apaudit_tally(aps, 0, &t);
    assert(t.flagged == 0 && t.grade_n[0] == 0 && t.grade_n[3] == 0);
}

int main(void)
{
    test_authmode_str();
    test_is_open();
    test_auth_grade();
    test_eapol_msg();
    test_pwnagotchi();
    test_mac_is_random();
    test_oui_vendor();
    test_wigle();
    test_airspace_tally();
    test_apaudit_tally();
    printf("test_wifi_labels: all tests passed\n");
    return 0;
}
