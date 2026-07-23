// lxveos_wifi_eapol — see lxveos_wifi_eapol.h. PURE hashcat-22000 line formatter, libc-only, host-tested
// off-target (tests/host_c/test_wifi_eapol.c). Split out of lxveos_wifi.c's promiscuous capture so the line
// format is verifiable without hardware.
#include "lxveos_wifi_eapol.h"

#include <stdio.h>   // snprintf
#include <string.h>  // strlen

// Append the lowercase hex of `len` bytes to out at offset *n, bounded by cap; out stays NUL-terminated. It
// stops early if the buffer is nearly full — the caller sizes out for the worst case, so this never trips in
// the real capture (it is a pure overflow guard).
static void append_hex(char *out, size_t cap, size_t *n, const uint8_t *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (*n + 3 > cap) {  // two hex digits + the NUL
            break;
        }
        *n += (size_t)snprintf(out + *n, cap - *n, "%02x", b[i]);
    }
}

// Append a literal string to out at *n, bounded by cap; out stays NUL-terminated.
static void append_str(char *out, size_t cap, size_t *n, const char *s)
{
    while (*s != '\0' && *n + 1 < cap) {
        out[(*n)++] = *s++;
    }
    if (*n < cap) {
        out[*n] = '\0';
    }
}

size_t lxveos_hc22000_pmkid(char *out, size_t cap, const uint8_t pmkid[16], const uint8_t ap[6],
                            const uint8_t sta[6], const char *essid)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (pmkid == NULL || ap == NULL || sta == NULL || essid == NULL || essid[0] == '\0') {
        return 0;  // empty ESSID (the PBKDF2 salt) is uncrackable — matches the capture's skip
    }
    size_t n = 0;
    append_str(out, cap, &n, "WPA*01*");
    append_hex(out, cap, &n, pmkid, 16);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, ap, 6);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, sta, 6);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, (const uint8_t *)essid, strlen(essid));
    append_str(out, cap, &n, "***");
    return n;
}

size_t lxveos_hc22000_eapol(char *out, size_t cap, const uint8_t mic[16], const uint8_t ap[6],
                            const uint8_t sta[6], const char *essid, const uint8_t anonce[32],
                            const uint8_t *eapol, size_t eapol_len, const char *messagepair)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (mic == NULL || ap == NULL || sta == NULL || essid == NULL || anonce == NULL ||
        eapol == NULL || messagepair == NULL || essid[0] == '\0') {
        return 0;
    }
    size_t n = 0;
    append_str(out, cap, &n, "WPA*02*");
    append_hex(out, cap, &n, mic, 16);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, ap, 6);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, sta, 6);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, (const uint8_t *)essid, strlen(essid));
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, anonce, 32);
    append_str(out, cap, &n, "*");
    append_hex(out, cap, &n, eapol, eapol_len);
    append_str(out, cap, &n, "*");
    append_str(out, cap, &n, messagepair);
    return n;
}
