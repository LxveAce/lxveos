#pragma once
// lxveos_evt — builds machine-readable event lines for the Cyber Controller serial bridge. Every line uses the
// same framing as the `LXVEOS/1 status` line CC already parses:
//
//     LXVEOS/1 <type> <key>=<value> <key>=<value> ...
//
// so CC's key/value tokenizer (which splits on spaces) stays unambiguous. Free-text / arbitrary-byte fields
// (SSIDs, names) are therefore emitted HEX-ENCODED via lxveos_evt_kv_hex() — never raw — so a space or control
// byte in an SSID can never break the line. The bridge stays human-glanceable but is primarily machine-read.
//
// The builders are a bounded accumulator: each takes the current write offset and returns the new one, never
// writing past `cap` and always keeping `buf` NUL-terminated. Dependency-free (libc only) so it host-tests with
// no ESP-IDF stubs (tests/host_c/test_evt.c). Typical use:
//
//     char line[256];
//     size_t n = lxveos_evt_begin(line, sizeof(line), "ap");
//     n = lxveos_evt_kv_mac(line, sizeof(line), n, "bssid", bssid);
//     n = lxveos_evt_kv_hex(line, sizeof(line), n, "ssid", ssid, ssid_len);
//     n = lxveos_evt_kv_int(line, sizeof(line), n, "ch", channel);
//     n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", rssi);
//     n = lxveos_evt_kv    (line, sizeof(line), n, "auth", "wpa2");
//     puts(line);   // LXVEOS/1 ap bssid=de:ad:be:ef:00:01 ssid=4d794e6574 ch=6 rssi=-42 auth=wpa2
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start an event line: writes "LXVEOS/1 <type>" into buf (capacity cap) and returns the new length. `type`
// should be a short lowercase token (ap/sta/probe/ble/hs/pcap/arm/alert/snapshot). Returns 0 (empty buf) on
// bad args (NULL/zero cap/empty type).
size_t lxveos_evt_begin(char *buf, size_t cap, const char *type);

// Append " key=value". `value` MUST already be token-safe (no spaces, newlines, or control bytes) — use this
// for enums/fixed tokens (auth modes, states); use lxveos_evt_kv_hex for anything free-text. Returns new length.
size_t lxveos_evt_kv(char *buf, size_t cap, size_t off, const char *key, const char *value);

// Append " key=<decimal>".
size_t lxveos_evt_kv_int(char *buf, size_t cap, size_t off, const char *key, long value);

// Append " key=<unsigned decimal>".
size_t lxveos_evt_kv_uint(char *buf, size_t cap, size_t off, const char *key, unsigned long value);

// Append " key=<lowercase hex of the len bytes>" — the safe encoding for SSIDs / names / any arbitrary bytes.
// An empty field (len 0) emits "key=" (CC decodes to an empty value). Returns new length.
size_t lxveos_evt_kv_hex(char *buf, size_t cap, size_t off, const char *key, const uint8_t *bytes, size_t len);

// Append " key=aa:bb:cc:dd:ee:ff" (lowercase, colon-separated) for a 6-byte MAC/BSSID.
size_t lxveos_evt_kv_mac(char *buf, size_t cap, size_t off, const char *key, const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
