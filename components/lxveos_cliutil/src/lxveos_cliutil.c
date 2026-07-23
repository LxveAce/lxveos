// lxveos_cliutil — see lxveos_cliutil.h. Dependency-free (libc-only) CLI helpers, host-tested off-target
// with no ESP-IDF toolchain (tests/host_c/test_cliutil.c).
#include "lxveos_cliutil.h"

#include <stdlib.h>  // strtol
#include <string.h>  // memcpy / memset

// One hex digit -> 0..15, or -1 if `c` is not a hex digit. Local so parse_mac stays libc-only.
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool parse_mac(const char *s, uint8_t out[6])
{
    if (s == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[0]);
        if (hi < 0) {
            return false;  // check s[0] BEFORE touching s[1], so we never read past the NUL
        }
        int lo = hex_nibble(s[1]);  // s[0] is a hex digit (not '\0'), so s[1] is in bounds (>= the NUL)
        if (lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i < 5) {
            if (*s != ':') {
                return false;  // octets 1..5 must be colon-separated
            }
            s++;
        }
    }
    return *s == '\0';  // reject anything trailing the sixth octet
}

bool parse_hex_octets(const char *s, uint8_t *out, size_t nbytes)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hex_nibble(s[0]);
        if (hi < 0) {
            return false;  // check s[0] BEFORE s[1] so a short string never reads past the NUL
        }
        int lo = hex_nibble(s[1]);
        if (lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return *s == '\0';  // reject any trailing chars (a too-long string)
}

bool parse_int_arg(const char *s, long lo, long hi, long *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v < lo || v > hi) {
        return false;  // no digits consumed, or out of the caller's range
    }
    *out = v;
    return true;
}

void sanitize_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    size_t i = 0;
    for (; src != NULL && src[i] != '\0' && i < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        // Fold C0 (< 0x20), DEL (0x7f) AND C1 (0x80-0x9f) control bytes to '.': a raw 0x9b is CSI on some
        // terminals. Bytes >= 0xa0 pass through so legitimate UTF-8 / high-Latin names survive.
        dst[i] = (c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f)) ? '.' : (char)c;
    }
    dst[i] = '\0';
}

void csv_quote_field(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (cap < 3) {  // no room for even the opening + closing quote of an empty field
        dst[0] = '\0';
        return;
    }
    size_t o = 0;
    dst[o++] = '"';
    // Invariant before writing content: always keep 2 slots free for the closing '"' and the NUL.
    for (size_t i = 0; src != NULL && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f)) {
            c = '.';  // sanitize C0/DEL/C1 control bytes -> a single '.' (>= 0xa0 UTF-8 passes through)
        }
        if (c == '"') {
            if (o + 2 > cap - 2) {
                break;  // RFC4180 doubling needs 2 chars; keep room for closing + NUL
            }
            dst[o++] = '"';
            dst[o++] = '"';
        } else {
            if (o + 1 > cap - 2) {
                break;
            }
            dst[o++] = (char)c;
        }
    }
    dst[o++] = '"';
    dst[o] = '\0';
}

size_t lxveos_watch_pack(const lxveos_watch_entry_t *entries, size_t n, uint8_t *buf, size_t cap)
{
    if (entries == NULL && n > 0) {
        return 0;
    }
    if (n > LXVEOS_WATCH_MAX) {
        n = LXVEOS_WATCH_MAX;  // never emit more entries than the format allows
    }
    size_t need = 2u + n * LXVEOS_WATCH_REC_SZ;
    if (buf == NULL || cap < need) {
        return 0;
    }
    buf[0] = (uint8_t)LXVEOS_WATCH_BLOB_VER;
    buf[1] = (uint8_t)n;
    uint8_t *p = buf + 2;
    for (size_t i = 0; i < n; i++) {
        memcpy(p, entries[i].mac, 6);
        uint8_t *lbl = p + 6;
        memset(lbl, 0, LXVEOS_WATCH_LABEL_CAP);  // zero-pad the fixed-width label field
        for (size_t k = 0; k + 1 < LXVEOS_WATCH_LABEL_CAP && entries[i].label[k] != '\0'; k++) {
            lbl[k] = (uint8_t)entries[i].label[k];  // copy up to CAP-1 bytes, always leaving a trailing NUL
        }
        p += LXVEOS_WATCH_REC_SZ;
    }
    return need;
}

size_t lxveos_watch_unpack(const uint8_t *buf, size_t len, lxveos_watch_entry_t *entries, size_t max)
{
    if (buf == NULL || entries == NULL || len < 2u) {
        return 0;
    }
    if (buf[0] != (uint8_t)LXVEOS_WATCH_BLOB_VER) {
        return 0;  // unknown format version -> treat as empty rather than guess at the layout
    }
    size_t declared = buf[1];
    if (declared > LXVEOS_WATCH_MAX) {
        declared = LXVEOS_WATCH_MAX;  // clamp a bogus count
    }
    size_t present = (len - 2u) / LXVEOS_WATCH_REC_SZ;  // whole records the buffer actually holds
    size_t n = declared < present ? declared : present;  // truncation-safe: never read past the data
    if (n > max) {
        n = max;
    }
    const uint8_t *p = buf + 2;
    for (size_t i = 0; i < n; i++) {
        memcpy(entries[i].mac, p, 6);
        memcpy(entries[i].label, p + 6, LXVEOS_WATCH_LABEL_CAP);
        entries[i].label[LXVEOS_WATCH_LABEL_CAP - 1] = '\0';  // guarantee termination even if the blob lied
        p += LXVEOS_WATCH_REC_SZ;
    }
    return n;
}
