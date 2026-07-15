// Host-side unit test for lxveos_radiomath (external-radio pure math). Pure libc, no ESP-IDF toolchain.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
//
// The point of B5 was to DEDUP + host-test math that used to live inline in three drivers. So each block
// recomputes the driver's original expression independently and asserts the extracted function matches it
// bit-for-bit — that is the "this is a refactor, not a behaviour change" proof — then pins a couple of
// hand-worked known vectors so a wrong formula can't pass by matching a wrong reference.
#include "lxveos_radiomath.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_cc1101_freq_to_word(void)
{
    // Reference = the exact expression the subghz driver used inline (two copies, now one). Sweep the band.
    const float xosc = 26000000.0f;
    for (float mhz = 300.0f; mhz <= 928.0f; mhz += 0.13f) {
        uint32_t ref = (uint32_t)((mhz * 1000000.0f) * 65536.0f / xosc);
        assert(lxveos_cc1101_freq_to_word(mhz) == ref);
    }
    // 433.92 MHz -> 433.92e6 * 65536 / 26e6 = 1,093,745.43 -> 1,093,745 (0x10B071). Allow a couple of ULP
    // of float32 slack rather than pinning an exact integer (the intermediate exceeds a 24-bit mantissa).
    // Sanity: 0x10/0xB0/0x71 is exactly the FREQ2/FREQ1/FREQ0 the driver's base config ships for 433.92 MHz.
    uint32_t w = lxveos_cc1101_freq_to_word(433.92f);
    assert(labs((long)w - 1093745L) <= 4);
}

static void test_cc1101_rssi_to_dbm(void)
{
    // Reference = the driver's inline conversion, over every possible raw byte.
    for (int r = 0; r < 256; r++) {
        int rssi_dec = (r >= 128) ? (r - 256) : r;
        int dbm = rssi_dec / 2 - 74;
        if (dbm < -128) dbm = -128;
        if (dbm > 127) dbm = 127;
        assert(lxveos_cc1101_rssi_to_dbm((uint8_t)r) == (int8_t)dbm);
    }
    // Hand-worked anchors (note C integer division truncates toward zero for the negatives).
    assert(lxveos_cc1101_rssi_to_dbm(0) == -74);      // 0/2 - 74
    assert(lxveos_cc1101_rssi_to_dbm(127) == -43);    // 63/2 - 74 = 31 - 74
    assert(lxveos_cc1101_rssi_to_dbm(128) == -128);   // (128-256)/2 - 74 = -64 - 74 = -138 -> clamp
    assert(lxveos_cc1101_rssi_to_dbm(200) == -102);   // (200-256)/2 - 74 = -28 - 74
    assert(lxveos_cc1101_rssi_to_dbm(255) == -74);    // (255-256)/2 - 74 = 0 - 74
}

static void test_pn532_frame(void)
{
    // SAMConfiguration: cmd = 14 01 14 00 (the driver's first transaction).
    const uint8_t cmd[] = {0x14, 0x01, 0x14, 0x00};
    uint8_t frame[32];
    size_t n = lxveos_pn532_build_frame(0xD4, cmd, (uint8_t)sizeof(cmd), frame, sizeof(frame));
    assert(n == sizeof(cmd) + 8);   // clen + 8
    // Hand-worked expected bytes: LEN=5, LCS=0xFB, DCS=0x03.
    const uint8_t want[] = {0x00, 0x00, 0xFF, 0x05, 0xFB, 0xD4, 0x14, 0x01, 0x14, 0x00, 0x03, 0x00};
    assert(n == sizeof(want) && memcmp(frame, want, n) == 0);
    // The checksum helpers agree with the built frame.
    assert(lxveos_pn532_lcs(0x05) == 0xFB);
    assert(lxveos_pn532_dcs(0xD4, cmd, (uint8_t)sizeof(cmd)) == 0x03);
    // A freshly-built frame validates; the empty-payload minimum (GetFirmwareVersion is 1 byte) too.
    assert(lxveos_pn532_frame_valid(frame, n));
    const uint8_t gfv[] = {0x02};
    n = lxveos_pn532_build_frame(0xD4, gfv, 1, frame, sizeof(frame));
    assert(n == 9 && lxveos_pn532_frame_valid(frame, n));

    // Rejections: bad preamble, bad LCS, bad DCS, bad postamble, truncated, too-small buffer.
    uint8_t bad[32];
    memcpy(bad, want, sizeof(want));
    bad[2] = 0xFE; assert(!lxveos_pn532_frame_valid(bad, sizeof(want)));     // preamble
    memcpy(bad, want, sizeof(want));
    bad[4] = 0x00; assert(!lxveos_pn532_frame_valid(bad, sizeof(want)));     // LCS
    memcpy(bad, want, sizeof(want));
    bad[10] = 0x04; assert(!lxveos_pn532_frame_valid(bad, sizeof(want)));    // DCS
    memcpy(bad, want, sizeof(want));
    bad[11] = 0x01; assert(!lxveos_pn532_frame_valid(bad, sizeof(want)));    // postamble
    assert(!lxveos_pn532_frame_valid(want, 7));                             // truncated (< 8)
    assert(!lxveos_pn532_frame_valid(NULL, 12));
    // Won't fit: needs clen+8 = 12, give 11.
    assert(lxveos_pn532_build_frame(0xD4, cmd, (uint8_t)sizeof(cmd), frame, 11) == 0);
    assert(lxveos_pn532_build_frame(0xD4, cmd, (uint8_t)sizeof(cmd), NULL, 99) == 0);
}

static void test_mifare_bcc(void)
{
    // BCC = XOR of the four UID bytes (the driver's block-0 check byte).
    const uint8_t uid[4] = {0x04, 0x1A, 0x2B, 0x3C};
    assert(lxveos_mifare_bcc4(uid) == (uint8_t)(0x04 ^ 0x1A ^ 0x2B ^ 0x3C));
    assert(lxveos_mifare_bcc4(uid) == 0x09);
    // Property: uid[0..3] XOR bcc == 0.
    assert((uid[0] ^ uid[1] ^ uid[2] ^ uid[3] ^ lxveos_mifare_bcc4(uid)) == 0);
    const uint8_t zero[4] = {0, 0, 0, 0};
    assert(lxveos_mifare_bcc4(zero) == 0x00);
}

static void test_unifying_checksum(void)
{
    // The driver's 10-byte Logitech Unifying keyboard frame: dev-idx, 0xC1, mod, key, 6 pad, checksum.
    // Reference = its inline loop: sum f[0..8], checksum = 0 - sum.
    uint8_t f[10] = {0x00, 0xC1, 0x00, 0x04, 0, 0, 0, 0, 0, 0};
    uint8_t sum = 0;
    for (int i = 0; i < 9; i++) sum = (uint8_t)(sum + f[i]);
    uint8_t ref = (uint8_t)(0 - sum);
    assert(lxveos_unifying_checksum(f, sizeof(f)) == ref);
    assert(ref == 0x3B);                                   // hand-worked: 0-(0xC1+0x04) = 0-0xC5
    // Fill the slot and the whole frame sums to zero.
    f[9] = lxveos_unifying_checksum(f, sizeof(f));
    assert(lxveos_unifying_checksum_ok(f, sizeof(f)));
    f[9] ^= 0x01;
    assert(!lxveos_unifying_checksum_ok(f, sizeof(f)));    // a one-bit corruption is caught
    // Guards.
    assert(lxveos_unifying_checksum(NULL, 10) == 0);
    assert(!lxveos_unifying_checksum_ok(NULL, 10));
    assert(!lxveos_unifying_checksum_ok(f, 0));
}

int main(void)
{
    test_cc1101_freq_to_word();
    test_cc1101_rssi_to_dbm();
    test_pn532_frame();
    test_mifare_bcc();
    test_unifying_checksum();
    printf("test_radiomath: all tests passed\n");
    return 0;
}
