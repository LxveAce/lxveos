#pragma once
// lxveos_baseline — dependency-free (libc-only) environment-fingerprint baseline: a sorted set of 6-byte device
// addresses (Wi-Fi BSSIDs or BLE addresses) plus a diff that reports what APPEARED / DISAPPEARED since a saved
// baseline, and a compact serializer to persist it. A counter-surveillance primitive: snapshot the devices in a
// known-safe place, then later diff a fresh scan to surface a newly-present tracker/camera (added) or a device
// that left (removed). Kept a standalone component so the set/diff/codec host-test off-target
// (tests/host_c/test_baseline.c). PURE — no radio here; the caller feeds it addresses from a Wi-Fi/BLE scan.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXVEOS_BASELINE_ADDR_LEN 6u  // a device address is 6 octets (BSSID / BLE MAC)

// Compare two 6-byte addresses in memcmp order (<0 / 0 / >0). The sort + dedup order the set and diff rely on.
int lxveos_baseline_cmp(const uint8_t a[6], const uint8_t b[6]);

// Insert `addr` into a SORTED baseline set (ascending lxveos_baseline_cmp order), keeping it sorted and
// deduplicated. No-op (returns `n` unchanged) if `addr` is already present or the set is full (n >= cap).
// Returns the new element count. O(n) — shifts the tail to keep the array sorted.
size_t lxveos_baseline_insert(uint8_t (*set)[6], size_t n, size_t cap, const uint8_t addr[6]);

// Diff two SORTED baseline sets. Addresses in `new_set` but not `old_set` are ADDED; addresses in `old_set` but
// not `new_set` are REMOVED. Writes up to `added_cap` / `removed_cap` of each into the output arrays and sets
// *n_added / *n_removed to the TRUE counts (which may exceed the caps — the caller detects truncation). Both
// inputs MUST be sorted + deduplicated (use lxveos_baseline_insert to build them). Pure, O(n_old + n_new). A
// NULL count pointer, or a NULL / zero-cap output array, is allowed — that side is still counted, just not written.
void lxveos_baseline_diff(const uint8_t (*old_set)[6], size_t n_old,
                          const uint8_t (*new_set)[6], size_t n_new,
                          uint8_t (*added)[6], size_t added_cap, size_t *n_added,
                          uint8_t (*removed)[6], size_t removed_cap, size_t *n_removed);

// Serialize a baseline set to a compact blob: a 2-byte big-endian count, then n * 6 address bytes. Returns the
// number of bytes written (2 + n*6), or 0 if `blob` is NULL, `n` exceeds 0xFFFF, or `blobcap` is too small.
size_t lxveos_baseline_serialize(const uint8_t (*set)[6], size_t n, uint8_t *blob, size_t blobcap);

// Load a baseline set from a blob written by lxveos_baseline_serialize (2-byte BE count + n*6 bytes). Writes up
// to `cap` addresses into `set` and returns the number written (<= cap). Returns 0 on a NULL/too-short blob or a
// malformed one (a declared count whose bytes don't fit `bloblen`). The stored order is preserved (sorted on save).
size_t lxveos_baseline_deserialize(const uint8_t *blob, size_t bloblen, uint8_t (*set)[6], size_t cap);

#ifdef __cplusplus
}
#endif
