#pragma once
// lxveos_cliutil — small, dependency-free (libc-only) helpers shared by the LxveOS CLI: validated
// integer-argument parsing and console-output sanitization of device-supplied text. Pulled out of
// lxveos_cli.c so this pure logic host-tests with no ESP-IDF toolchain (tests/host_c/test_cliutil.c).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse a MAC / BLE address "aa:bb:cc:dd:ee:ff" (upper- or lower-case hex, colon-separated) into out[6].
// Returns false unless the whole string is EXACTLY six two-hex-digit octets separated by single colons (no
// short octets, no wrong separator, no trailing garbage). NULL args return false.
bool parse_mac(const char *s, uint8_t out[6]);

// Parse exactly `nbytes` CONTIGUOUS two-hex-digit octets (2*nbytes chars, no separators) from `s` into
// out[0..nbytes-1] — e.g. parse_hex_octets("DEADBEEF", out, 4) -> {0xDE,0xAD,0xBE,0xEF}. Returns false
// unless the whole string is exactly that many hex digits: a short/long string, a non-hex digit, or any
// trailing char all reject (so a mistyped NFC UID can't be silently truncated). NULL args return false.
bool parse_hex_octets(const char *s, uint8_t *out, size_t nbytes);

// Parse a base-10 integer CLI argument in [lo, hi] into *out. Returns false for NULL args, a non-numeric
// token (no digits consumed), or one out of range — so a bad GPIO / seconds arg errors with a usage hint
// instead of silently becoming 0 the way atoi() does. A trailing-garbage token like "8x" parses as 8
// (digits-then-stop), matching the convention the Wi-Fi ops already use.
bool parse_int_arg(const char *s, long lo, long hi, long *out);

// Copy `src` into `dst` (at most cap-1 bytes) with control/non-printable bytes (< 0x20 or 0x7f) replaced by
// '.', so a DEVICE-SUPPLIED string (a Wi-Fi SSID, a BLE local name) can't emit terminal escapes that garble
// or spoof the operator console when printed via a raw %s. `dst` is always NUL-terminated; cap 0 and a NULL
// dst are no-ops; a NULL src yields an empty string.
void sanitize_copy(char *dst, size_t cap, const char *src);

// Format `src` as one RFC4180 CSV field into `dst`: a double-quoted field with any embedded '"' doubled and
// control bytes (< 0x20 or 0x7f) replaced by '.', so a comma / quote / newline in a device-supplied name
// can't break the CSV row or inject a new one. `dst` always includes the closing quote and a NUL (content is
// truncated first if `cap` is tight); a cap < 3 (no room for `""`) yields an empty string.
void csv_quote_field(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif
