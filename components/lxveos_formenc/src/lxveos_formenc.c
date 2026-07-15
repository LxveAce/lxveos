// lxveos_formenc — see lxveos_formenc.h. Pure (libc-only) form-submission text helpers, extracted verbatim
// from the evil-portal op so the untrusted-input parsers are unit-testable on the host without ESP-IDF stubs.
// The only behavioural addition over the originals is a `dstsz == 0` guard on the two writers, appropriate
// now that they are a shared public API rather than file-static helpers with trusted call sites.
#include "lxveos_formenc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void lxveos_formenc_url_decode(const char *src, char *dst, size_t dstsz)
{
    if (dstsz == 0) {
        return;
    }
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dstsz; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char h[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char)strtol(h, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

bool lxveos_formenc_form_field(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            char raw[160];
            if (vlen >= sizeof(raw)) {
                vlen = sizeof(raw) - 1;
            }
            memcpy(raw, val, vlen);
            raw[vlen] = '\0';
            lxveos_formenc_url_decode(raw, out, outsz);
            return true;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

void lxveos_formenc_sanitize(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) {
            *s = '.';
        }
    }
}

void lxveos_formenc_store_field(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}
