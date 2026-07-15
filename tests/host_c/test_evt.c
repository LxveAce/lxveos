// Host-side unit test for lxveos_evt — the "LXVEOS/1 <type> k=v ..." serial-bridge event-line builder that the
// Cyber Controller parser binds to. The exact wire format matters (CC splits on spaces and hex-decodes free-text
// fields), so this asserts byte-exact output, the hex/MAC encodings, and — critically — that a small buffer is
// never overrun (the builder is used with fixed stack buffers on-device). Dependency-free: plain libc, no stubs.
#include <stdio.h>
#include <string.h>

#include "lxveos_evt.h"

static int g_fail = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

static void test_full_ap_line(void)
{
    char b[256];
    size_t n = lxveos_evt_begin(b, sizeof(b), "ap");
    CHECK(strcmp(b, "LXVEOS/1 ap") == 0);

    uint8_t bssid[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    n = lxveos_evt_kv_mac(b, sizeof(b), n, "bssid", bssid);

    uint8_t ssid[] = {'M', 'y', 'N', 'e', 't'};  // 4d 79 4e 65 74
    n = lxveos_evt_kv_hex(b, sizeof(b), n, "ssid", ssid, sizeof(ssid));

    n = lxveos_evt_kv_int(b, sizeof(b), n, "ch", 6);
    n = lxveos_evt_kv_int(b, sizeof(b), n, "rssi", -42);
    n = lxveos_evt_kv(b, sizeof(b), n, "auth", "wpa2");

    CHECK(strcmp(b, "LXVEOS/1 ap bssid=de:ad:be:ef:00:01 ssid=4d794e6574 ch=6 rssi=-42 auth=wpa2") == 0);
    CHECK(n == strlen(b));  // returned offset tracks the real length
}

static void test_field_encodings(void)
{
    char b[128];
    size_t n;

    // Empty hex field emits "key=" (CC decodes to empty).
    n = lxveos_evt_begin(b, sizeof(b), "x");
    n = lxveos_evt_kv_hex(b, sizeof(b), n, "ssid", NULL, 0);
    CHECK(strcmp(b, "LXVEOS/1 x ssid=") == 0);

    // Unsigned + a hidden/space-containing SSID hex-encodes safely (space 0x20 -> "20").
    n = lxveos_evt_begin(b, sizeof(b), "ap");
    uint8_t ss[] = {'a', ' ', 'b'};  // 61 20 62 — the space would break a space-split parser if raw
    n = lxveos_evt_kv_hex(b, sizeof(b), n, "ssid", ss, sizeof(ss));
    n = lxveos_evt_kv_uint(b, sizeof(b), n, "heap", 123456UL);
    CHECK(strcmp(b, "LXVEOS/1 ap ssid=612062 heap=123456") == 0);

    // Negative rssi and a plain token value.
    n = lxveos_evt_begin(b, sizeof(b), "sta");
    n = lxveos_evt_kv_int(b, sizeof(b), n, "rssi", -91);
    n = lxveos_evt_kv(b, sizeof(b), n, "state", "assoc");
    CHECK(strcmp(b, "LXVEOS/1 sta rssi=-91 state=assoc") == 0);
}

static void test_bounds_no_overrun(void)
{
    // Give a 32-byte buffer but tell the builder cap=10, then overflow it. Bytes at/after index 10 must be
    // untouched (proves nothing was written past cap), and the string stays NUL-terminated within cap.
    char big[32];
    memset(big, 'Z', sizeof(big));
    size_t n = lxveos_evt_begin(big, 10, "ap");            // "LXVEOS/1 ap" is 11 > 9 -> truncates
    n = lxveos_evt_kv(big, 10, n, "ssid", "overflowme");   // no room left -> no-op
    n = lxveos_evt_kv_int(big, 10, n, "ch", 11);           // still no-op
    CHECK(strlen(big) <= 9);        // never exceeded cap-1
    CHECK(big[9] == '\0');          // NUL at the boundary
    CHECK(big[10] == 'Z');          // the byte just past cap is pristine — no overrun
    CHECK(big[31] == 'Z');
    CHECK(n <= 9);
}

static void test_bad_args(void)
{
    char b[32];
    CHECK(lxveos_evt_begin(NULL, 10, "ap") == 0);
    CHECK(lxveos_evt_begin(b, 0, "ap") == 0);
    b[0] = 'x';
    CHECK(lxveos_evt_begin(b, sizeof(b), NULL) == 0 && b[0] == '\0');
    b[0] = 'x';
    CHECK(lxveos_evt_begin(b, sizeof(b), "") == 0 && b[0] == '\0');

    // Appenders with NULL key/value leave the offset unchanged.
    size_t n = lxveos_evt_begin(b, sizeof(b), "ap");
    CHECK(lxveos_evt_kv(b, sizeof(b), n, NULL, "v") == n);
    CHECK(lxveos_evt_kv(b, sizeof(b), n, "k", NULL) == n);
    CHECK(lxveos_evt_kv_mac(b, sizeof(b), n, "bssid", NULL) == n);
    CHECK(strcmp(b, "LXVEOS/1 ap") == 0);
}

int main(void)
{
    test_full_ap_line();
    test_field_encodings();
    test_bounds_no_overrun();
    test_bad_args();
    printf(g_fail ? "evt host tests: FAILED\n" : "evt host tests: OK\n");
    return g_fail;
}
