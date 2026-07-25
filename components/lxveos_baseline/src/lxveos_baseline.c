// lxveos_baseline — see the header. Pure libc-only baseline set / diff / codec, host-tested off-target
// (tests/host_c/test_baseline.c). No radio, no ESP-IDF: the caller supplies addresses from a Wi-Fi/BLE scan.
#include "lxveos_baseline.h"

#include <string.h>  // memcmp, memcpy, memmove

int lxveos_baseline_cmp(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, LXVEOS_BASELINE_ADDR_LEN);
}

size_t lxveos_baseline_insert(uint8_t (*set)[6], size_t n, size_t cap, const uint8_t addr[6])
{
    if (set == NULL || addr == NULL || n >= cap) {
        return n;  // full set or bad args -> unchanged
    }
    // Locate the sorted insertion point; bail on an exact match (dedup).
    size_t i = 0;
    for (; i < n; i++) {
        int c = lxveos_baseline_cmp(set[i], addr);
        if (c == 0) {
            return n;  // already present
        }
        if (c > 0) {
            break;  // addr sorts before set[i] -> insert here
        }
    }
    if (i < n) {  // open a slot at i by shifting the [i, n) tail up one (skip when appending at the end)
        memmove(set[i + 1], set[i], (n - i) * LXVEOS_BASELINE_ADDR_LEN);
    }
    memcpy(set[i], addr, LXVEOS_BASELINE_ADDR_LEN);
    return n + 1;
}

void lxveos_baseline_diff(const uint8_t (*old_set)[6], size_t n_old,
                          const uint8_t (*new_set)[6], size_t n_new,
                          uint8_t (*added)[6], size_t added_cap, size_t *n_added,
                          uint8_t (*removed)[6], size_t removed_cap, size_t *n_removed)
{
    size_t i = 0, j = 0, na = 0, nr = 0;
    while (i < n_old && j < n_new) {
        int c = lxveos_baseline_cmp(old_set[i], new_set[j]);
        if (c == 0) {  // present in both -> unchanged
            i++;
            j++;
        } else if (c < 0) {  // old_set[i] absent from new -> removed
            if (removed != NULL && nr < removed_cap) {
                memcpy(removed[nr], old_set[i], LXVEOS_BASELINE_ADDR_LEN);
            }
            nr++;
            i++;
        } else {  // new_set[j] absent from old -> added
            if (added != NULL && na < added_cap) {
                memcpy(added[na], new_set[j], LXVEOS_BASELINE_ADDR_LEN);
            }
            na++;
            j++;
        }
    }
    for (; i < n_old; i++) {  // whatever remains in old is removed
        if (removed != NULL && nr < removed_cap) {
            memcpy(removed[nr], old_set[i], LXVEOS_BASELINE_ADDR_LEN);
        }
        nr++;
    }
    for (; j < n_new; j++) {  // whatever remains in new is added
        if (added != NULL && na < added_cap) {
            memcpy(added[na], new_set[j], LXVEOS_BASELINE_ADDR_LEN);
        }
        na++;
    }
    if (n_added != NULL) {
        *n_added = na;
    }
    if (n_removed != NULL) {
        *n_removed = nr;
    }
}

size_t lxveos_baseline_serialize(const uint8_t (*set)[6], size_t n, uint8_t *blob, size_t blobcap)
{
    if (blob == NULL || (n > 0 && set == NULL) || n > 0xFFFFu) {
        return 0;
    }
    size_t need = 2 + n * LXVEOS_BASELINE_ADDR_LEN;
    if (blobcap < need) {
        return 0;
    }
    blob[0] = (uint8_t)((n >> 8) & 0xFFu);  // big-endian element count
    blob[1] = (uint8_t)(n & 0xFFu);
    for (size_t k = 0; k < n; k++) {
        memcpy(&blob[2 + k * LXVEOS_BASELINE_ADDR_LEN], set[k], LXVEOS_BASELINE_ADDR_LEN);
    }
    return need;
}

size_t lxveos_baseline_deserialize(const uint8_t *blob, size_t bloblen, uint8_t (*set)[6], size_t cap)
{
    if (blob == NULL || set == NULL || bloblen < 2) {
        return 0;
    }
    size_t declared = ((size_t)blob[0] << 8) | (size_t)blob[1];
    if (bloblen < 2 + declared * LXVEOS_BASELINE_ADDR_LEN) {
        return 0;  // malformed: the declared count doesn't fit the blob
    }
    size_t out = declared < cap ? declared : cap;  // never write past the caller's array
    for (size_t k = 0; k < out; k++) {
        memcpy(set[k], &blob[2 + k * LXVEOS_BASELINE_ADDR_LEN], LXVEOS_BASELINE_ADDR_LEN);
    }
    return out;
}
