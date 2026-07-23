// Host-side unit test for lxveos_wifi_eapol (the pure hashcat-22000 WPA*01/WPA*02 line formatter). Pure libc,
// no ESP-IDF toolchain. Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assert.
#include "lxveos_wifi_eapol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_pmkid_line(void)
{
    uint8_t pmkid[16];
    for (int i = 0; i < 16; i++) {
        pmkid[i] = (uint8_t)i;  // 000102...0f
    }
    const uint8_t ap[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    const uint8_t sta[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    char out[256];

    // ESSID "Net" -> 4e6574; the line ends with the three empty 22000 fields (***).
    size_t n = lxveos_hc22000_pmkid(out, sizeof(out), pmkid, ap, sta, "Net");
    const char *want =
        "WPA*01*000102030405060708090a0b0c0d0e0f*deadbeef0001*112233445566*4e6574***";
    assert(strcmp(out, want) == 0);
    assert(n == strlen(want));

    // an empty ESSID is uncrackable -> 0, and out is cleared to ""
    out[0] = 'X';
    assert(lxveos_hc22000_pmkid(out, sizeof(out), pmkid, ap, sta, "") == 0);
    assert(out[0] == '\0');

    // NULL args are rejected, not dereferenced
    assert(lxveos_hc22000_pmkid(NULL, sizeof(out), pmkid, ap, sta, "Net") == 0);
    assert(lxveos_hc22000_pmkid(out, sizeof(out), NULL, ap, sta, "Net") == 0);
}

static void test_eapol_line(void)
{
    uint8_t mic[16];
    for (int i = 0; i < 16; i++) {
        mic[i] = (uint8_t)(0xa0 + i);  // a0a1...af
    }
    const uint8_t ap[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    const uint8_t sta[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t anonce[32];
    for (int i = 0; i < 32; i++) {
        anonce[i] = (uint8_t)i;  // 0001...1f
    }
    const uint8_t eapol[4] = {0x01, 0x03, 0x00, 0x5f};
    char out[1024];

    size_t n = lxveos_hc22000_eapol(out, sizeof(out), mic, ap, sta, "Net", anonce, eapol, 4, "00");
    const char *want =
        "WPA*02*a0a1a2a3a4a5a6a7a8a9aaabacadaeaf*deadbeef0001*112233445566*4e6574*"
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f*0103005f*00";
    assert(strcmp(out, want) == 0);
    assert(n == strlen(want));

    // the messagepair is emitted verbatim (M3+M4 pairs report "05")
    n = lxveos_hc22000_eapol(out, sizeof(out), mic, ap, sta, "Net", anonce, eapol, 4, "05");
    assert(strcmp(out + n - 3, "*05") == 0);

    // empty ESSID -> uncrackable -> 0
    assert(lxveos_hc22000_eapol(out, sizeof(out), mic, ap, sta, "", anonce, eapol, 4, "00") == 0);
    assert(out[0] == '\0');
}

int main(void)
{
    test_pmkid_line();
    test_eapol_line();
    printf("test_wifi_eapol: all assertions passed\n");
    return 0;
}
