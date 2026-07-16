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

int main(void)
{
    test_nec();
    test_sony();
    test_unknown_and_null();
    printf("test_ir_decode: all tests passed\n");
    return 0;
}
