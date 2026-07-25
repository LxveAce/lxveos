// Host-side unit test for lxveos_baseline (environment-fingerprint set / diff / codec). Pure libc, no stubs.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_baseline.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_cmp(void)
{
    const uint8_t a[6] = {0, 0, 0, 0, 0, 1};
    const uint8_t b[6] = {0, 0, 0, 0, 0, 2};
    assert(lxveos_baseline_cmp(a, b) < 0);
    assert(lxveos_baseline_cmp(b, a) > 0);
    assert(lxveos_baseline_cmp(a, a) == 0);
}

static void test_insert(void)
{
    uint8_t set[8][6] = {{0}};
    size_t n = 0;
    const uint8_t c[6] = {0xCC, 0, 0, 0, 0, 0};
    const uint8_t a[6] = {0xAA, 0, 0, 0, 0, 0};
    const uint8_t b[6] = {0xBB, 0, 0, 0, 0, 0};
    // Insert out of order -> the set stays sorted ascending.
    n = lxveos_baseline_insert(set, n, 8, c); assert(n == 1);
    n = lxveos_baseline_insert(set, n, 8, a); assert(n == 2);
    n = lxveos_baseline_insert(set, n, 8, b); assert(n == 3);
    assert(memcmp(set[0], a, 6) == 0);
    assert(memcmp(set[1], b, 6) == 0);
    assert(memcmp(set[2], c, 6) == 0);
    // Dedup: re-inserting an existing address is a no-op.
    n = lxveos_baseline_insert(set, n, 8, b); assert(n == 3);
    n = lxveos_baseline_insert(set, n, 8, a); assert(n == 3);

    // Full set: inserting when n >= cap is rejected, leaving the set intact.
    uint8_t small[2][6] = {{0}};
    size_t m = 0;
    m = lxveos_baseline_insert(small, m, 2, a); assert(m == 1);
    m = lxveos_baseline_insert(small, m, 2, b); assert(m == 2);
    m = lxveos_baseline_insert(small, m, 2, c); assert(m == 2);  // full -> unchanged
    assert(memcmp(small[0], a, 6) == 0 && memcmp(small[1], b, 6) == 0);

    // NULL args are safe.
    assert(lxveos_baseline_insert(NULL, 0, 8, a) == 0);
    assert(lxveos_baseline_insert(set, n, 8, NULL) == n);
}

static void test_diff(void)
{
    const uint8_t A[6] = {0x0A, 0, 0, 0, 0, 0};
    const uint8_t B[6] = {0x0B, 0, 0, 0, 0, 0};
    const uint8_t C[6] = {0x0C, 0, 0, 0, 0, 0};
    const uint8_t D[6] = {0x0D, 0, 0, 0, 0, 0};
    uint8_t added[4][6], removed[4][6];
    size_t na = 99, nr = 99;

    // old = {A, B, C}; new = {B, C, D}  ->  added {D}, removed {A}.
    uint8_t oldset[3][6]; memcpy(oldset[0], A, 6); memcpy(oldset[1], B, 6); memcpy(oldset[2], C, 6);
    uint8_t newset[3][6]; memcpy(newset[0], B, 6); memcpy(newset[1], C, 6); memcpy(newset[2], D, 6);
    lxveos_baseline_diff(oldset, 3, newset, 3, added, 4, &na, removed, 4, &nr);
    assert(na == 1 && memcmp(added[0], D, 6) == 0);
    assert(nr == 1 && memcmp(removed[0], A, 6) == 0);

    // Identical sets -> nothing changed.
    lxveos_baseline_diff(oldset, 3, oldset, 3, added, 4, &na, removed, 4, &nr);
    assert(na == 0 && nr == 0);

    // Disjoint sets -> all-new added, all-old removed.
    uint8_t s1[2][6]; memcpy(s1[0], A, 6); memcpy(s1[1], B, 6);
    uint8_t s2[2][6]; memcpy(s2[0], C, 6); memcpy(s2[1], D, 6);
    lxveos_baseline_diff(s1, 2, s2, 2, added, 4, &na, removed, 4, &nr);
    assert(na == 2 && memcmp(added[0], C, 6) == 0 && memcmp(added[1], D, 6) == 0);
    assert(nr == 2 && memcmp(removed[0], A, 6) == 0 && memcmp(removed[1], B, 6) == 0);

    // Empty old -> everything in new is added; empty new -> everything in old is removed.
    lxveos_baseline_diff(NULL, 0, s2, 2, added, 4, &na, removed, 4, &nr);
    assert(na == 2 && nr == 0);
    lxveos_baseline_diff(s1, 2, NULL, 0, added, 4, &na, removed, 4, &nr);
    assert(na == 0 && nr == 2);

    // Truncation: the TRUE count is reported even when the output cap is smaller (only cap items written).
    lxveos_baseline_diff(s1, 2, s2, 2, added, 1, &na, removed, 1, &nr);
    assert(na == 2 && nr == 2);
    assert(memcmp(added[0], C, 6) == 0);
    assert(memcmp(removed[0], A, 6) == 0);

    // NULL count pointers + NULL / zero-cap output arrays are safe (counted, not written).
    lxveos_baseline_diff(s1, 2, s2, 2, NULL, 0, NULL, NULL, 0, NULL);
}

static void test_serialize(void)
{
    const uint8_t A[6] = {0x0A, 1, 2, 3, 4, 5};
    const uint8_t B[6] = {0x0B, 6, 7, 8, 9, 10};
    uint8_t set[2][6]; memcpy(set[0], A, 6); memcpy(set[1], B, 6);
    uint8_t blob[64];
    uint8_t out[4][6];

    size_t len = lxveos_baseline_serialize(set, 2, blob, sizeof(blob));
    assert(len == 2 + 2 * 6);
    assert(blob[0] == 0x00 && blob[1] == 0x02);  // big-endian count = 2
    assert(memcmp(&blob[2], A, 6) == 0 && memcmp(&blob[8], B, 6) == 0);

    // Round-trip.
    size_t m = lxveos_baseline_deserialize(blob, len, out, 4);
    assert(m == 2 && memcmp(out[0], A, 6) == 0 && memcmp(out[1], B, 6) == 0);

    // Empty set -> just the 2-byte count header; deserializes to 0 addresses.
    len = lxveos_baseline_serialize(set, 0, blob, sizeof(blob));
    assert(len == 2 && blob[0] == 0 && blob[1] == 0);
    assert(lxveos_baseline_deserialize(blob, len, out, 4) == 0);

    // Too-small blob on serialize -> 0 (needs 14, has 8).
    uint8_t tiny[8];
    assert(lxveos_baseline_serialize(set, 2, tiny, sizeof(tiny)) == 0);

    // Malformed deserialize: a declared count whose bytes don't fit -> 0.
    uint8_t bad[8] = {0x00, 0x02, 1, 2, 3, 4, 5, 6};  // claims 2 addrs (needs 14) but only 8 bytes
    assert(lxveos_baseline_deserialize(bad, sizeof(bad), out, 4) == 0);
    // Short blob (< 2 bytes) -> 0.
    assert(lxveos_baseline_deserialize(blob, 1, out, 4) == 0);

    // Deserialize honours the caller's cap: a 2-addr blob into a cap-1 array writes only 1, never overruns.
    len = lxveos_baseline_serialize(set, 2, blob, sizeof(blob));
    m = lxveos_baseline_deserialize(blob, len, out, 1);
    assert(m == 1 && memcmp(out[0], A, 6) == 0);

    // NULL args are safe.
    assert(lxveos_baseline_serialize(set, 2, NULL, 0) == 0);
    assert(lxveos_baseline_deserialize(NULL, 14, out, 4) == 0);
}

int main(void)
{
    test_cmp();
    test_insert();
    test_diff();
    test_serialize();
    printf("test_baseline: all tests passed\n");
    return 0;
}
