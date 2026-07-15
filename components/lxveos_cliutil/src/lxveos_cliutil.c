// lxveos_cliutil — see lxveos_cliutil.h. Dependency-free (libc-only) CLI helpers, host-tested off-target
// with no ESP-IDF toolchain (tests/host_c/test_cliutil.c).
#include "lxveos_cliutil.h"

#include <stdlib.h>  // strtol

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
        dst[i] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;
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
        if (c < 0x20 || c == 0x7f) {
            c = '.';  // sanitize control bytes -> a single '.' (not a quote, so it takes the else branch)
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
