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

// Copy `src` into `dst` (at most cap-1 bytes) with control bytes (C0 < 0x20, DEL 0x7f, and C1 0x80-0x9f)
// replaced by '.', so a DEVICE-SUPPLIED string (a Wi-Fi SSID, a BLE local name) can't emit terminal escapes
// that garble or spoof the operator console when printed via a raw %s. Bytes >= 0xa0 pass through so
// legitimate UTF-8 / high-Latin names survive. `dst` is always NUL-terminated; cap 0 and a NULL dst are
// no-ops; a NULL src yields an empty string.
void sanitize_copy(char *dst, size_t cap, const char *src);

// Format `src` as one RFC4180 CSV field into `dst`: a double-quoted field with any embedded '"' doubled and
// control bytes (C0 < 0x20, DEL 0x7f, C1 0x80-0x9f) replaced by '.', so a comma / quote / newline in a device-supplied name
// can't break the CSV row or inject a new one. `dst` always includes the closing quote and a NUL (content is
// truncated first if `cap` is tight); a cap < 3 (no room for `""`) yields an empty string.
void csv_quote_field(char *dst, size_t cap, const char *src);

// --- target-watchlist persistence codec ------------------------------------------------------------------
// Pack/unpack for the CLI `watch` list so it survives a reboot via an NVS blob. Pure/libc-only, so it
// host-tests off-target; the NVS glue that calls it lives in the CLI and is compile-checked by the board
// matrix. The entry type is shared with the CLI so the codec packs the live list directly, no conversion.
#define LXVEOS_WATCH_MAX        16u  // max targets held (the CLI's WATCH_MAX aliases this)
#define LXVEOS_WATCH_LABEL_CAP  24   // NUL-terminated operator note per target (the CLI's WATCH_LABEL)

typedef struct {
    uint8_t mac[6];                    // target address, MSB-first (the order `scan` / `blescan` display)
    char    label[LXVEOS_WATCH_LABEL_CAP];  // optional operator note ("" if none), always NUL-terminated
} lxveos_watch_entry_t;

// Wire format: [ver=1][count][count x {6 mac bytes, 24 label bytes NUL-padded}]. Fixed-width so pack/unpack
// stay trivial and a short/corrupt blob truncates cleanly instead of misparsing a partial record.
#define LXVEOS_WATCH_BLOB_VER   0x01u
#define LXVEOS_WATCH_REC_SZ     (6u + LXVEOS_WATCH_LABEL_CAP)                   // 30 bytes per entry
#define LXVEOS_WATCH_BLOB_MAX   (2u + LXVEOS_WATCH_MAX * LXVEOS_WATCH_REC_SZ)   // 482-byte max blob

// Serialize the first min(n, LXVEOS_WATCH_MAX) entries into buf. Returns the byte count written (>= 2, since
// an empty list still writes the 2-byte header), or 0 if buf is NULL, entries is NULL with n > 0, or cap is
// too small to hold that many entries.
size_t lxveos_watch_pack(const lxveos_watch_entry_t *entries, size_t n, uint8_t *buf, size_t cap);

// Parse a blob produced by lxveos_watch_pack into entries[0..max-1]. Returns the entry count recovered
// (<= max and <= LXVEOS_WATCH_MAX). A NULL/short blob, an unknown version byte, or a declared count larger
// than the bytes actually present all yield only the fully-present entries (0 for a bad header), so a
// truncated or corrupt NVS value can never over-read or load a partial record. Each returned label is
// guaranteed NUL-terminated.
size_t lxveos_watch_unpack(const uint8_t *buf, size_t len, lxveos_watch_entry_t *entries, size_t max);

#ifdef __cplusplus
}
#endif
