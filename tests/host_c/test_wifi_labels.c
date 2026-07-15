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

int main(void)
{
    test_authmode_str();
    test_is_open();
    test_auth_grade();
    printf("test_wifi_labels: all tests passed\n");
    return 0;
}
