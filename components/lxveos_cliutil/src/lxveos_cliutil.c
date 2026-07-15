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
