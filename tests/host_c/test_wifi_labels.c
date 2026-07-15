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
}

int main(void)
{
    test_authmode_str();
    test_is_open();
    test_auth_grade();
    test_pwnagotchi();
    printf("test_wifi_labels: all tests passed\n");
    return 0;
}
