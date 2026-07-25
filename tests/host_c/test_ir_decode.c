// Host-side unit test for lxveos_ir_decode (pure IR NEC/Sony protocol decode). libc + the esp_err stub that
// lxveos_ir.h pulls in — no RMT driver. Built + run by tests/host_c/run.sh. Aborts (non-zero) on first failure.
#include "lxveos_ir.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Build a NEC duration train (9ms/4.5ms lead-in, 32 LSB-first bits, stop mark) into d[]; returns its length.
static size_t build_nec(uint16_t *d, uint8_t addr, uint8_t addr_inv, uint8_t cmd, uint8_t cmd_inv)
{
    size_t k = 0;
    d[k++] = 9000;
    d[k++] = 4500;
    uint32_t val = (uint32_t)addr | ((uint32_t)addr_inv << 8) |
                   ((uint32_t)cmd << 16) | ((uint32_t)cmd_inv << 24);
    for (int b = 0; b < 32; b++) {
        d[k++] = 560;
        d[k++] = ((val >> b) & 1u) ? 1690 : 560;
    }
    d[k++] = 560;   // stop mark
    return k;
}

// Build a Sony SIRC train (2.4ms start, then `bits` LSB-first bits, each a mark + 600µs space) into d[].
static size_t build_sony(uint16_t *d, uint32_t val, int bits)
{
    size_t k = 0;
    d[k++] = 2400;
    d[k++] = 600;
    for (int b = 0; b < bits; b++) {
        d[k++] = ((val >> b) & 1u) ? 1200 : 600;
        d[k++] = 600;
    }
    return k;
}

// Build a canonical Philips RC5 mark/space train for (addr, cmd, toggle) into d[]: assemble the 14-bit frame
// (S1=1, S2 = inverted 7th command bit, toggle, 5 address bits, low 6 command bits), expand to 28 Manchester
// half-bits ('1' = space then mark, '0' = mark then space), drop S1's implicit leading space + a '0'-last-bit's
// trailing space, then run-length merge into 889/1778µs durations. Written straight from the RC5 spec and kept
// separate from the source encoder, so it independently exercises the decoder and cross-checks lxveos_ir_encode.
static size_t build_rc5(uint16_t *d, uint8_t addr, uint8_t cmd, uint8_t toggle)
{
    uint8_t s2 = (cmd & 0x40u) ? 0u : 1u;
    uint16_t val = (uint16_t)(((uint16_t)1u << 13) | ((uint16_t)s2 << 12) | ((uint16_t)(toggle & 1u) << 11)
                              | ((uint16_t)(addr & 0x1Fu) << 6) | (uint16_t)(cmd & 0x3Fu));
    uint8_t hb[28];
    for (int k = 0; k < 14; k++) {
        int bit = (val >> (13 - k)) & 1u;
        hb[2 * k] = (uint8_t)(bit ? 0 : 1);
        hb[2 * k + 1] = (uint8_t)(bit ? 1 : 0);
    }
    int hi = (hb[27] == 0u) ? 27 : 28;
    size_t k = 0;
    int i = 1;
    while (i < hi) {
        int lvl = hb[i];
        int run = 1;
        while (i + run < hi && hb[i + run] == lvl) {
            run++;
        }
        d[k++] = (uint16_t)(run * 889);
        i += run;
    }
    return k;
}

static void test_nec(void)
{
    uint16_t d[128];
    lxveos_ir_decoded_t dec;

    // Standard NEC: addr 0x04, cmd 0x08, both bytes properly inverted.
    size_t n = build_nec(d, 0x04, 0xFB, 0x08, 0xF7);
    assert(lxveos_ir_decode(d, n, &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_NEC);
    assert(dec.address == 0x04);
    assert(dec.command == 0x08);
    assert(dec.addr_ext == false);
    assert(dec.bits == 32);
    assert(strcmp(lxveos_ir_proto_str(dec.proto), "NEC") == 0);

    // NEC-extended: the address byte is NOT its own inverse -> 16-bit address, addr_ext set.
    n = build_nec(d, 0x04, 0x10, 0x08, 0xF7);
    assert(lxveos_ir_decode(d, n, &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_NEC);
    assert(dec.address == 0x1004);   // addr | (addr_inv << 8)
    assert(dec.addr_ext == true);
    assert(dec.command == 0x08);

    // Command integrity check must fail a bad frame (cmd not the inverse of cmd_inv) -> rejected.
    n = build_nec(d, 0x04, 0xFB, 0x08, 0x00);
    assert(lxveos_ir_decode(d, n, &dec) == false);
    assert(dec.proto == LXVEOS_IR_PROTO_UNKNOWN);

    // NEC repeat code (held button): 9ms mark + 2.25ms space + stop mark.
    uint16_t rep[3] = {9000, 2250, 560};
    assert(lxveos_ir_decode(rep, 3, &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_NEC_REPEAT);
    assert(dec.bits == 0);
    assert(strcmp(lxveos_ir_proto_str(dec.proto), "NEC-repeat") == 0);

    // A NEC lead-in with too few bits is not a confident decode.
    uint16_t shortnec[6] = {9000, 4500, 560, 560, 560, 1690};
    assert(lxveos_ir_decode(shortnec, 6, &dec) == false);
}

static void test_sony(void)
{
    uint16_t d[64];
    lxveos_ir_decoded_t dec;

    // 12-bit SIRC: command 0x12 (7 bits) + address 5 (5 bits).
    uint32_t val = (uint32_t)0x12 | ((uint32_t)5 << 7);
    size_t n = build_sony(d, val, 12);
    assert(lxveos_ir_decode(d, n, &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_SONY);
    assert(dec.command == 0x12);
    assert(dec.address == 5);
    assert(dec.bits == 12);
    assert(strcmp(lxveos_ir_proto_str(dec.proto), "Sony") == 0);

    // 20-bit SIRC: command 0x7F + a wider address.
    val = (uint32_t)0x7F | ((uint32_t)0x1AB << 7);
    n = build_sony(d, val, 20);
    assert(lxveos_ir_decode(d, n, &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_SONY);
    assert(dec.command == 0x7F);
    assert(dec.address == 0x1AB);
    assert(dec.bits == 20);

    // A non-canonical bit count (13) is rejected.
    n = build_sony(d, val, 13);
    assert(lxveos_ir_decode(d, n, &dec) == false);
}

static void test_rc5(void)
{
    uint16_t d[40];
    lxveos_ir_decoded_t dec;

    // Plain RC5: address 5, command 0x35 (6-bit, bit 6 clear -> S2=1). The toggle bit must not affect addr/cmd.
    for (int tg = 0; tg <= 1; tg++) {
        size_t n = build_rc5(d, 5, 0x35, (uint8_t)tg);
        assert(lxveos_ir_decode(d, n, &dec) == true);
        assert(dec.proto == LXVEOS_IR_PROTO_RC5);
        assert(dec.address == 5);
        assert(dec.command == 0x35);
        assert(dec.bits == 14);
        assert(dec.addr_ext == false);
        assert(strcmp(lxveos_ir_proto_str(dec.proto), "RC5") == 0);
    }

    // RC5X extended command (bit 6 set -> a 7-bit command 0x40..0x7F, carried by S2=0).
    size_t n = build_rc5(d, 0x1F, 0x66, 0);
    assert(lxveos_ir_decode(d, n, &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_RC5 && dec.address == 0x1F && dec.command == 0x66);

    // Boundary values: all-zero, and the maximum 5-bit address / 6-bit command.
    n = build_rc5(d, 0, 0, 0);
    assert(lxveos_ir_decode(d, n, &dec) && dec.address == 0 && dec.command == 0);
    n = build_rc5(d, 0x1F, 0x3F, 0);
    assert(lxveos_ir_decode(d, n, &dec) && dec.address == 0x1F && dec.command == 0x3F);

    // build_rc5 and the source encoder build the same spec-defined train, so this is a cheap DRIFT guard that
    // the two stay in step (the independent correctness check is the golden vectors below), not a second oracle.
    uint16_t enc[40];
    size_t en = 0;
    lxveos_ir_decoded_t in = {LXVEOS_IR_PROTO_RC5, 5, 0x35, 14, false};
    n = build_rc5(d, 5, 0x35, 0);
    assert(lxveos_ir_encode(&in, enc, 40, &en));
    assert(en == n && memcmp(d, enc, n * sizeof(uint16_t)) == 0);

    // Golden vectors: literal mark/space trains produced by a SEPARATE (Python) RC5 encoder written straight
    // from the spec — frozen here so decode is validated against a fixed external reference, not only its own
    // inverse. golden_5_35 = addr 5 / cmd 0x35 (plain RC5); golden_1f_7f = addr 0x1F / cmd 0x7F (max RC5X).
    static const uint16_t golden_5_35[] = {889, 889, 1778, 889, 889, 889, 889, 1778, 1778, 1778,
                                           889, 889, 889, 889, 1778, 1778, 1778, 1778, 889};
    assert(lxveos_ir_decode(golden_5_35, sizeof(golden_5_35) / sizeof(golden_5_35[0]), &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_RC5 && dec.address == 5 && dec.command == 0x35 && dec.bits == 14);
    static const uint16_t golden_1f_7f[] = {1778, 889, 889, 1778, 889, 889, 889, 889, 889, 889, 889, 889, 889,
                                            889, 889, 889, 889, 889, 889, 889, 889, 889, 889, 889, 889};
    assert(lxveos_ir_decode(golden_1f_7f, sizeof(golden_1f_7f) / sizeof(golden_1f_7f[0]), &dec) == true);
    assert(dec.proto == LXVEOS_IR_PROTO_RC5 && dec.address == 0x1F && dec.command == 0x7F);

    // A corrupted duration (neither 1T nor 2T) breaks the RC5 decode.
    n = build_rc5(d, 5, 0x35, 0);
    d[2] = 5000;
    assert(lxveos_ir_decode(d, n, &dec) == false);

    // A too-short all-1T train cannot form a 14-bit frame (wrong half-bit count) -> rejected, not accepted.
    uint16_t tooshort[5] = {889, 889, 889, 889, 889};
    assert(lxveos_ir_decode(tooshort, 5, &dec) == false);
}

static void test_unknown_and_null(void)
{
    lxveos_ir_decoded_t dec;
    // Random/short trains decode to nothing.
    uint16_t noise[8] = {100, 200, 100, 300, 150, 220, 90, 400};
    assert(lxveos_ir_decode(noise, 8, &dec) == false);
    assert(dec.proto == LXVEOS_IR_PROTO_UNKNOWN);
    uint16_t one[1] = {9000};
    assert(lxveos_ir_decode(one, 1, &dec) == false);

    // NULL args are safe.
    assert(lxveos_ir_decode(NULL, 0, &dec) == false);
    assert(lxveos_ir_decode(noise, 8, NULL) == false);

    // UNKNOWN has no label.
    assert(lxveos_ir_proto_str(LXVEOS_IR_PROTO_UNKNOWN) == NULL);
}

// The encoder is the inverse of the decoder, so the strongest check is a round-trip: encode a command, decode
// the durations it produced, and require the decode to reproduce the original fields exactly.
static void test_encode_roundtrip(void)
{
    uint16_t buf[80];
    size_t n = 0;
    lxveos_ir_decoded_t in, dec;

    // NEC, standard 8-bit address
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_NEC, 0x04, 0x08, 32, false};
    assert(lxveos_ir_encode(&in, buf, sizeof(buf) / sizeof(buf[0]), &n));
    assert(lxveos_ir_decode(buf, n, &dec));
    assert(dec.proto == LXVEOS_IR_PROTO_NEC && dec.address == 0x04 && dec.command == 0x08
           && dec.bits == 32 && dec.addr_ext == false);

    // NEC, extended 16-bit address (0x34 ^ 0x12 != 0xFF, so it stays extended through the round-trip)
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_NEC, 0x1234, 0x56, 32, true};
    assert(lxveos_ir_encode(&in, buf, 80, &n) && lxveos_ir_decode(buf, n, &dec));
    assert(dec.proto == LXVEOS_IR_PROTO_NEC && dec.address == 0x1234 && dec.command == 0x56
           && dec.addr_ext == true);

    // NEC boundary commands (the inverted-byte integrity check must still pass)
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_NEC, 0x00, 0xFF, 32, false};
    assert(lxveos_ir_encode(&in, buf, 80, &n) && lxveos_ir_decode(buf, n, &dec));
    assert(dec.address == 0x00 && dec.command == 0xFF);

    // NEC repeat code
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_NEC_REPEAT, 0, 0, 0, false};
    assert(lxveos_ir_encode(&in, buf, 80, &n) && n == 2);
    assert(lxveos_ir_decode(buf, n, &dec) && dec.proto == LXVEOS_IR_PROTO_NEC_REPEAT);

    // Sony, all three canonical bit lengths
    const int slens[] = {12, 15, 20};
    for (int i = 0; i < 3; i++) {
        in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_SONY, 0x0A, 0x15, (uint8_t)slens[i], false};
        assert(lxveos_ir_encode(&in, buf, 80, &n));
        assert(lxveos_ir_decode(buf, n, &dec));
        assert(dec.proto == LXVEOS_IR_PROTO_SONY && dec.address == 0x0A && dec.command == 0x15
               && dec.bits == slens[i]);
    }

    // RC5: a plain 6-bit command, and an RC5X 7-bit command (bit 6 set), each across all 32 addresses.
    for (int a = 0; a < 32; a++) {
        in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_RC5, (uint16_t)a, 0x2A, 14, false};
        assert(lxveos_ir_encode(&in, buf, 80, &n) && lxveos_ir_decode(buf, n, &dec));
        assert(dec.proto == LXVEOS_IR_PROTO_RC5 && dec.address == (uint16_t)a && dec.command == 0x2A
               && dec.bits == 14);
        in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_RC5, (uint16_t)a, 0x71, 14, false};   // RC5X (7-bit command)
        assert(lxveos_ir_encode(&in, buf, 80, &n) && lxveos_ir_decode(buf, n, &dec));
        assert(dec.proto == LXVEOS_IR_PROTO_RC5 && dec.address == (uint16_t)a && dec.command == 0x71);
    }
    // RC5 command boundaries (last-bit '0' vs '1', which exercise the trailing-space handling both ways).
    const uint16_t rc5cmds[] = {0x00, 0x01, 0x3E, 0x3F, 0x40, 0x7F};
    for (int i = 0; i < 6; i++) {
        in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_RC5, 0x15, rc5cmds[i], 14, false};
        assert(lxveos_ir_encode(&in, buf, 80, &n) && lxveos_ir_decode(buf, n, &dec));
        assert(dec.proto == LXVEOS_IR_PROTO_RC5 && dec.address == 0x15 && dec.command == rc5cmds[i]);
    }

    // rejections: unsupported proto, a non-canonical Sony length, a too-small buffer, and NULL args
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_UNKNOWN, 0, 0, 0, false};
    assert(!lxveos_ir_encode(&in, buf, 80, &n));
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_SONY, 0, 0, 13, false};
    assert(!lxveos_ir_encode(&in, buf, 80, &n));
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_NEC, 0x04, 0x08, 32, false};
    assert(!lxveos_ir_encode(&in, buf, 4, &n));  // buffer too small for a NEC frame
    in = (lxveos_ir_decoded_t){LXVEOS_IR_PROTO_RC5, 5, 0x35, 14, false};
    assert(!lxveos_ir_encode(&in, buf, 10, &n));  // buffer too small for an RC5 frame
    assert(!lxveos_ir_encode(NULL, buf, 80, &n));
}

int main(void)
{
    test_nec();
    test_sony();
    test_rc5();
    test_unknown_and_null();
    test_encode_roundtrip();
    printf("test_ir_decode: all tests passed\n");
    return 0;
}
