#pragma once
// lxveos_formenc — dependency-free (libc-only) helpers for handling untrusted form-submission text: the
// captive-portal POST bodies that the evil-portal op parses from client-supplied input. Kept out of the
// I/O-heavy evilportal translation unit so the parsers can be unit-tested on the host with no ESP-IDF stubs
// (tests/host_c/test_formenc.c). store_field() is also the canonical GCC-15 -Werror-safe bounded copy
// (memcpy, not strncpy / snprintf("%s")) that other components should reuse instead of re-deriving it.
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode application/x-www-form-urlencoded text ('+' -> space, %XX -> byte) from `src` into the bounded,
// always-NUL-terminated `dst` (capacity `dstsz`). A '%' not followed by two hex digits is copied literally,
// and decoded NUL bytes are preserved (so `dst` may contain embedded NULs). No-op if `dstsz` is 0.
void lxveos_formenc_url_decode(const char *src, char *dst, size_t dstsz);

// Extract form field `key` from urlencoded `body` into `out` (URL-decoded, bounded by `outsz`). Returns
// false and leaves `out` untouched when the key is absent. Matches only a whole `key=` token, so a request
// for "user" does not match "username=".
bool lxveos_formenc_form_field(const char *body, const char *key, char *out, size_t outsz);

// Replace control bytes (< 0x20, and DEL 0x7f) in-place with '.' so a crafted submission can't garble the
// console log.
void lxveos_formenc_sanitize(char *s);

// Bounded copy of NUL-terminated `src` into `dst` (capacity `dstsz`): never overflows, always NUL-terminates.
// The GCC-15 -Werror build treats snprintf("%s") truncation as fatal and strncpy can leave a buffer
// unterminated, so this memcpy form is the safe idiom. No-op if `dstsz` is 0.
void lxveos_formenc_store_field(char *dst, size_t dstsz, const char *src);

#ifdef __cplusplus
}
#endif
