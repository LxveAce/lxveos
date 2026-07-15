#pragma once
// lxveos_cliutil — small, dependency-free (libc-only) helpers shared by the LxveOS CLI: validated
// integer-argument parsing and console-output sanitization of device-supplied text. Pulled out of
// lxveos_cli.c so this pure logic host-tests with no ESP-IDF toolchain (tests/host_c/test_cliutil.c).
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
