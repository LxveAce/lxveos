// lxveos_cfg — see lxveos_cfg.h. Dependency-free escaped-text config codec, host-tested off-target
// (tests/host_c/test_cfg.c). libc-only: string.h, no allocation.
#include "lxveos_cfg.h"

#include <string.h>  // memcpy

// Append `s` to out at offset *o, backslash-escaping the three structural bytes. Returns false if it would
// overflow (leaving room for the caller's trailing separator + a final NUL is the caller's job).
static bool esc_append(const char *s, char *out, size_t cap, size_t *o)
{
    for (; *s != '\0'; s++) {
        char c = *s;
        char e = 0;
        if (c == '\\') {
            e = '\\';
        } else if (c == '\t') {
            e = 't';
        } else if (c == '\n') {
            e = 'n';
        } else if ((unsigned char)c < 0x20) {
            return false;  // other control bytes aren't representable (parse rejects them symmetrically)
        }
        if (e != 0) {
            if (*o + 2 >= cap) {  // 2 chars + leave room for a NUL
                return false;
            }
            out[(*o)++] = '\\';
            out[(*o)++] = e;
        } else {
            if (*o + 1 >= cap) {
                return false;
            }
            out[(*o)++] = c;
        }
    }
    return true;
}

size_t lxveos_cfg_serialize(const lxveos_cfg_row_t *rows, size_t n, char *out, size_t cap)
{
    if (rows == NULL || out == NULL || cap == 0 || n > LXVEOS_CFG_ROWS_MAX) {
        return 0;
    }
    size_t o = 0;
    for (size_t r = 0; r < n; r++) {
        if (!esc_append(rows[r].key, out, cap, &o)) {
            return 0;
        }
        if (o + 1 >= cap) {
            return 0;
        }
        out[o++] = '\t';  // key/value separator
        if (!esc_append(rows[r].value, out, cap, &o)) {
            return 0;
        }
        if (o + 1 >= cap) {
            return 0;
        }
        out[o++] = '\n';  // row terminator
    }
    if (o >= cap) {
        return 0;
    }
    out[o] = '\0';
    return o;
}

size_t lxveos_cfg_parse(const char *blob, size_t len, lxveos_cfg_row_t *rows, size_t max)
{
    if (blob == NULL || rows == NULL) {
        return 0;
    }
    size_t nr = 0;
    size_t i = 0;
    while (i < len && nr < max && nr < LXVEOS_CFG_ROWS_MAX) {
        char key[LXVEOS_CFG_KEY_MAX];
        char val[LXVEOS_CFG_VAL_MAX];
        size_t kn = 0, vn = 0;
        bool in_value = false;
        bool ok = true;
        bool row_end = false;
        while (i < len) {
            char c = blob[i++];
            if (c == '\n') {  // raw newline terminates the row
                row_end = true;
                break;
            }
            if (c == '\t' && !in_value) {  // raw tab separates key from value (once)
                in_value = true;
                continue;
            }
            char decoded = 0;
            if (c == '\\') {
                if (i >= len) {
                    ok = false;
                    break;
                }
                char e = blob[i++];
                if (e == '\\') {
                    decoded = '\\';
                } else if (e == 't') {
                    decoded = '\t';
                } else if (e == 'n') {
                    decoded = '\n';
                } else {
                    ok = false;  // unknown escape
                    break;
                }
            } else if ((unsigned char)c < 0x20) {
                ok = false;  // a raw control byte (tab/newline were handled above) is malformed
                break;
            } else {
                decoded = c;
            }
            if (!in_value) {
                if (kn + 1 >= LXVEOS_CFG_KEY_MAX) {
                    ok = false;
                    break;
                }
                key[kn++] = decoded;
            } else {
                if (vn + 1 >= LXVEOS_CFG_VAL_MAX) {
                    ok = false;
                    break;
                }
                val[vn++] = decoded;
            }
        }
        // A well-formed row needs a key/value separator and a terminating newline (the last row too).
        if (!ok || !in_value || !row_end) {
            return 0;
        }
        key[kn] = '\0';
        val[vn] = '\0';
        memcpy(rows[nr].key, key, kn + 1);
        memcpy(rows[nr].value, val, vn + 1);
        nr++;
    }
    return nr;
}
