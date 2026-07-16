// lxveos_ir_decode — pure IR protocol decode (NEC / Sony SIRC), split out of lxveos_ir.c so it can be
// host-unit-tested (tests/host_c/test_ir_decode.c) without the ESP-IDF RMT driver. It works on a flat mark/
// space duration train (microseconds); the RMT-coupled glue that produces that train from a live capture
// (lxveos_ir_decode_last) stays in lxveos_ir.c. libc-only, no allocation.
#include "lxveos_ir.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// True if `v` is within +/- tol_pct percent of `target` (all microseconds). Consumer IR receivers stretch/
// shrink marks vs spaces by a few hundred µs, so the tolerances below are deliberately generous but still
// keep NEC's '0' (~560µs) and '1' (~1690µs) spaces, and Sony's '0' (~600µs) and '1' (~1200µs) marks, apart.
static bool near_us(uint16_t v, uint16_t target, uint8_t tol_pct)
{
    uint32_t tol = (uint32_t)target * tol_pct / 100u;
    uint32_t lo = (target > tol) ? (uint32_t)target - tol : 0u;
    uint32_t hi = (uint32_t)target + tol;
    return (uint32_t)v >= lo && (uint32_t)v <= hi;
}

// NEC: 9ms leading mark + 4.5ms space, then 32 bits (each a 560µs mark + a space: ~560µs = '0', ~1690µs =
// '1', LSB-first), then a stop mark. The 32 bits are addr, ~addr, cmd, ~cmd. A 9ms mark + 2.25ms space is
// the repeat code (held button). Returns true (filling *out) only when the command's inverted-byte check
// passes — that self-check is what makes a NEC decode high-confidence / low false-positive.
static bool decode_nec(const uint16_t *d, size_t n, lxveos_ir_decoded_t *out)
{
    if (n < 2) {
        return false;
    }
    if (!near_us(d[0], 9000, 25)) {
        return false;
    }
    if (near_us(d[1], 2250, 25)) {   // repeat code (no payload)
        out->proto = LXVEOS_IR_PROTO_NEC_REPEAT;
        out->address = 0;
        out->command = 0;
        out->bits = 0;
        out->addr_ext = false;
        return true;
    }
    if (!near_us(d[1], 4500, 20)) {
        return false;
    }
    if (n < 2 + 64) {   // need 32 mark/space pairs after the 2-symbol lead-in
        return false;
    }
    uint32_t val = 0;
    for (int b = 0; b < 32; b++) {
        uint16_t mark = d[2 + 2 * b];
        uint16_t space = d[3 + 2 * b];
        if (!near_us(mark, 560, 40)) {
            return false;
        }
        if (near_us(space, 1690, 25)) {
            val |= (1u << b);        // LSB-first
        } else if (!near_us(space, 560, 40)) {
            return false;            // neither a '0' nor a '1' space -> not NEC
        }
    }
    uint8_t addr = (uint8_t)(val & 0xFFu);
    uint8_t addr_inv = (uint8_t)((val >> 8) & 0xFFu);
    uint8_t cmd = (uint8_t)((val >> 16) & 0xFFu);
    uint8_t cmd_inv = (uint8_t)((val >> 24) & 0xFFu);
    if ((uint8_t)(cmd ^ cmd_inv) != 0xFF) {
        return false;                // command integrity check failed -> reject rather than mis-decode
    }
    out->proto = LXVEOS_IR_PROTO_NEC;
    out->command = cmd;
    out->bits = 32;
    if ((uint8_t)(addr ^ addr_inv) == 0xFF) {
        out->address = addr;         // standard 8-bit address
        out->addr_ext = false;
    } else {
        out->address = (uint16_t)(addr | ((uint16_t)addr_inv << 8));  // NEC-extended 16-bit address
        out->addr_ext = true;
    }
    return true;
}

// Sony SIRC: 2.4ms start mark + 600µs space, then N bits, each a mark (1.2ms = '1', 600µs = '0') followed by
// a 600µs space, LSB-first. Only the marks carry data, so we sample marks at even offsets and accept the
// canonical 12/15/20-bit lengths. The low 7 bits are the command; the rest are the address (+ extended).
static bool decode_sony(const uint16_t *d, size_t n, lxveos_ir_decoded_t *out)
{
    if (n < 2) {
        return false;
    }
    if (!near_us(d[0], 2400, 25) || !near_us(d[1], 600, 35)) {
        return false;
    }
    uint32_t val = 0;
    int bits = 0;
    for (int b = 0; b < 20; b++) {
        size_t mi = 2 + (size_t)2 * b;
        if (mi >= n) {
            break;
        }
        uint16_t mark = d[mi];
        if (near_us(mark, 1200, 30)) {
            val |= (1u << b);
        } else if (!near_us(mark, 600, 35)) {
            break;                   // not a Sony bit mark -> end of frame
        }
        bits++;
    }
    if (bits != 12 && bits != 15 && bits != 20) {
        return false;                // only the three canonical SIRC lengths count as a confident decode
    }
    out->proto = LXVEOS_IR_PROTO_SONY;
    out->bits = (uint8_t)bits;
    out->command = (uint16_t)(val & 0x7Fu);   // low 7 bits
    out->address = (uint16_t)(val >> 7);      // remaining bits (device, + extended on 20-bit)
    out->addr_ext = false;
    return true;
}

bool lxveos_ir_decode(const uint16_t *durations, size_t n, lxveos_ir_decoded_t *out)
{
    if (durations == NULL || out == NULL) {
        return false;
    }
    out->proto = LXVEOS_IR_PROTO_UNKNOWN;
    out->address = 0;
    out->command = 0;
    out->bits = 0;
    out->addr_ext = false;
    if (decode_nec(durations, n, out)) {
        return true;
    }
    if (decode_sony(durations, n, out)) {
        return true;
    }
    out->proto = LXVEOS_IR_PROTO_UNKNOWN;   // a sub-decoder may have half-filled *out before rejecting
    out->address = 0;
    out->command = 0;
    out->bits = 0;
    out->addr_ext = false;
    return false;
}

const char *lxveos_ir_proto_str(lxveos_ir_proto_t proto)
{
    switch (proto) {
    case LXVEOS_IR_PROTO_NEC:        return "NEC";
    case LXVEOS_IR_PROTO_NEC_REPEAT: return "NEC-repeat";
    case LXVEOS_IR_PROTO_SONY:       return "Sony";
    default:                         return NULL;
    }
}
