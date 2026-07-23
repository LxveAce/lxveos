// Host-side unit test for lxveos_wifi_essidmap (the pure BSSID->ESSID map + de-cloak upsert). Pure libc, no
// ESP-IDF toolchain. Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assert.
#include "lxveos_wifi_essidmap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX LXVEOS_ESSID_MAX

static void test_upsert_and_decloak(void)
{
    lxveos_essid_ent_t map[MAX];
    memset(map, 0, sizeof(map));
    const uint8_t ap1[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    const uint8_t ap2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    // a visible AP inserts, and lookups return its name
    assert(lxveos_essid_upsert(map, MAX, ap1, (const uint8_t *)"HomeNet", 7) == LXVEOS_ESSID_INSERTED);
    assert(strcmp(lxveos_essid_lookup(map, MAX, ap1), "HomeNet") == 0);
    // re-seeing a known AP never changes or downgrades the name (an evil-twin SSID cannot overwrite it)
    assert(lxveos_essid_upsert(map, MAX, ap1, (const uint8_t *)"Evil", 4) == LXVEOS_ESSID_NONE);
    assert(strcmp(lxveos_essid_lookup(map, MAX, ap1), "HomeNet") == 0);

    // a hidden AP inserts an empty entry; lookup is "" until it is revealed
    assert(lxveos_essid_upsert(map, MAX, ap2, (const uint8_t *)"", 0) == LXVEOS_ESSID_INSERTED);
    assert(strcmp(lxveos_essid_lookup(map, MAX, ap2), "") == 0);
    // a second hidden beacon for the same BSSID is a no-op
    assert(lxveos_essid_upsert(map, MAX, ap2, (const uint8_t *)"", 0) == LXVEOS_ESSID_NONE);
    // the real SSID leaks -> REVEALED (this is the first-write-wins bug fix / the de-cloak)
    assert(lxveos_essid_upsert(map, MAX, ap2, (const uint8_t *)"SecretAP", 8) == LXVEOS_ESSID_REVEALED);
    assert(strcmp(lxveos_essid_lookup(map, MAX, ap2), "SecretAP") == 0);
    // once revealed it is stable: no downgrade to hidden, no overwrite by another name
    assert(lxveos_essid_upsert(map, MAX, ap2, (const uint8_t *)"", 0) == LXVEOS_ESSID_NONE);
    assert(lxveos_essid_upsert(map, MAX, ap2, (const uint8_t *)"Other", 5) == LXVEOS_ESSID_NONE);
    assert(strcmp(lxveos_essid_lookup(map, MAX, ap2), "SecretAP") == 0);
}

static void test_bounds_and_full_map(void)
{
    lxveos_essid_ent_t map[MAX];
    memset(map, 0, sizeof(map));
    const uint8_t b[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // invalid lengths and a NULL ssid with len > 0 are rejected, not stored
    assert(lxveos_essid_upsert(map, MAX, b, (const uint8_t *)"x", -1) == LXVEOS_ESSID_NONE);
    assert(lxveos_essid_upsert(map, MAX, b, (const uint8_t *)"x", 33) == LXVEOS_ESSID_NONE);
    assert(lxveos_essid_upsert(map, MAX, b, NULL, 5) == LXVEOS_ESSID_NONE);

    // a max-length (32-byte) SSID stores and stays NUL-terminated
    char big[32];
    memset(big, 'A', sizeof(big));
    assert(lxveos_essid_upsert(map, MAX, b, (const uint8_t *)big, 32) == LXVEOS_ESSID_INSERTED);
    assert(strlen(lxveos_essid_lookup(map, MAX, b)) == 32);

    // fill the rest of the map, then an unknown BSSID with no free slot returns NONE
    for (int i = 1; i < MAX; i++) {
        const uint8_t bb[6] = {0xaa, 0x00, 0x00, 0x00, 0x00, (uint8_t)i};
        assert(lxveos_essid_upsert(map, MAX, bb, (const uint8_t *)"n", 1) == LXVEOS_ESSID_INSERTED);
    }
    const uint8_t overflow[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    assert(lxveos_essid_upsert(map, MAX, overflow, (const uint8_t *)"n", 1) == LXVEOS_ESSID_NONE);
    assert(strcmp(lxveos_essid_lookup(map, MAX, overflow), "") == 0);  // unknown -> ""
}

int main(void)
{
    test_upsert_and_decloak();
    test_bounds_and_full_map();
    printf("test_wifi_essidmap: all assertions passed\n");
    return 0;
}
