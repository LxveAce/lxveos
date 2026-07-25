// lxveos_ir_decode — pure IR protocol decode + encode (NEC / Sony SIRC), split out of lxveos_ir.c so it can be
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

// Philips RC5 (+ RC5X extended): 14 bits, bi-phase (Manchester) coded at an 889µs half-bit (T). Each bit is two
// half-bits — a '1' is a space(T) then mark(T) (low->high mid-bit transition), a '0' is a mark(T) then space(T)
// — so the mark/space train carries only runs of T (~889µs) or 2T (~1778µs). Frame, MSB first: S1 S2 T A4..A0
// C5..C0. S1 is always 1; S2 is 1 for plain RC5 or the inverted 7th command bit for RC5X (extending commands to
// 0..127); T is the (ignored) toggle. The receiver never captures the leading half-bit — it is S1's space half,
// carrier only starts at S1's mark — so we prepend an implicit leading space; likewise a frame ending on a '0'
// loses its trailing space to idle, so we append one when the reconstructed half-bit count comes out odd. The
// per-bit inverse-phase check + the fixed 14-bit length + S1==1 keep the decode low-false-positive. Ref: the
// public Philips RC5 protocol spec.
#define LXVEOS_RC5_T 889u
static bool decode_rc5(const uint16_t *d, size_t n, lxveos_ir_decoded_t *out)
{
    if (n < 2) {
        return false;
    }
    // Reconstruct the half-bit level sequence (0 = space / carrier off, 1 = mark / carrier on). hb[0] is the
    // implicit leading space (idle before S1's mark). d[0] is a MARK, then the levels alternate.
    uint8_t hb[30];
    size_t hc = 0;
    hb[hc++] = 0u;
    for (size_t i = 0; i < n; i++) {
        uint8_t level = (uint8_t)((i % 2u == 0u) ? 1u : 0u);   // d[0] MARK, then alternating SPACE/MARK
        size_t units;
        if (near_us(d[i], LXVEOS_RC5_T, 30)) {
            units = 1u;
        } else if (near_us(d[i], 2u * LXVEOS_RC5_T, 30)) {
            units = 2u;
        } else {
            return false;   // not a 1T / 2T RC5 duration
        }
        for (size_t u = 0; u < units; u++) {
            if (hc >= sizeof(hb)) {
                return false;   // longer than any 14-bit RC5 frame -> not RC5
            }
            hb[hc++] = level;
        }
    }
    if (hc == 27u) {
        hb[hc++] = 0u;   // frame ended on a '0': its trailing space fell into idle, so restore it
    }
    if (hc != 28u) {
        return false;    // an RC5 frame is exactly 28 half-bits (14 bits)
    }
    uint16_t val = 0;
    for (size_t k = 0; k < 14u; k++) {
        uint8_t first = hb[2u * k];
        uint8_t second = hb[2u * k + 1u];
        uint8_t bit;
        if (first == 0u && second == 1u) {
            bit = 1u;   // space then mark = '1'
        } else if (first == 1u && second == 0u) {
            bit = 0u;   // mark then space = '0'
        } else {
            return false;   // a (mark,mark) or (space,space) pair is not valid bi-phase -> not RC5
        }
        val = (uint16_t)((val << 1) | bit);
    }
    if (((val >> 13) & 1u) != 1u) {
        return false;   // S1 (the first start bit) is always 1
    }
    uint8_t s2 = (uint8_t)((val >> 12) & 1u);
    uint8_t addr = (uint8_t)((val >> 6) & 0x1Fu);   // A4..A0
    uint8_t cmd = (uint8_t)(val & 0x3Fu);           // C5..C0
    if (s2 == 0u) {
        cmd = (uint8_t)(cmd | 0x40u);   // RC5X: S2 = inverted 7th command bit -> S2 low means command 64..127
    }
    out->proto = LXVEOS_IR_PROTO_RC5;
    out->address = addr;
    out->command = cmd;
    out->bits = 14u;
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
    if (decode_rc5(durations, n, out)) {
        return true;
    }
    out->proto = LXVEOS_IR_PROTO_UNKNOWN;   // a sub-decoder may have half-filled *out before rejecting
    out->address = 0;
    out->command = 0;
    out->bits = 0;
    out->addr_ext = false;
    return false;
}

// Inverse of lxveos_ir_decode: build the mark/space duration train for a decoded command. Emits the exact
// canonical timings the decoder centres its tolerance windows on, so decode(encode(x)) == x for every
// supported command. NEC frames carry a trailing stop mark (the decoder reads only the lead-in + 32 bit
// pairs and ignores it).
bool lxveos_ir_encode(const lxveos_ir_decoded_t *in, uint16_t *out, size_t cap, size_t *n_out)
{
    if (in == NULL || out == NULL || n_out == NULL) {
        return false;
    }
    size_t n = 0;
    switch (in->proto) {
    case LXVEOS_IR_PROTO_NEC_REPEAT:
        if (cap < 2) {
            return false;
        }
        out[n++] = 9000;   // 9 ms lead mark
        out[n++] = 2250;   // 2.25 ms space = the repeat code (no payload)
        break;
    case LXVEOS_IR_PROTO_NEC: {
        // 32 payload bits, LSB-first: addr, addr-inverse (or the high address byte when extended), cmd, ~cmd.
        uint8_t cmd = (uint8_t)(in->command & 0xFFu);
        uint8_t addr_lo = (uint8_t)(in->address & 0xFFu);
        uint8_t addr_hi = in->addr_ext ? (uint8_t)((in->address >> 8) & 0xFFu) : (uint8_t)~addr_lo;
        uint32_t val = (uint32_t)addr_lo | ((uint32_t)addr_hi << 8) |
                       ((uint32_t)cmd << 16) | ((uint32_t)(uint8_t)~cmd << 24);
        if (cap < 2 + 64 + 1) {
            return false;
        }
        out[n++] = 9000;   // 9 ms lead mark
        out[n++] = 4500;   // 4.5 ms space
        for (int b = 0; b < 32; b++) {
            out[n++] = 560;                                  // bit mark
            out[n++] = (val & (1u << b)) ? 1690 : 560;       // '1' = long space, '0' = short space
        }
        out[n++] = 560;    // stop mark
        break;
    }
    case LXVEOS_IR_PROTO_SONY: {
        int bits = in->bits;
        if (bits != 12 && bits != 15 && bits != 20) {
            return false;
        }
        // Low 7 bits = command, the rest = address, LSB-first across the whole word.
        uint32_t val = ((uint32_t)in->address << 7) | (uint32_t)(in->command & 0x7Fu);
        if (cap < 2 + (size_t)2 * (size_t)bits) {
            return false;
        }
        out[n++] = 2400;   // 2.4 ms start mark
        out[n++] = 600;    // 600 us space
        for (int b = 0; b < bits; b++) {
            out[n++] = (val & (1u << b)) ? 1200 : 600;       // '1' = long mark, '0' = short mark
            out[n++] = 600;                                  // 600 us space
        }
        break;
    }
    case LXVEOS_IR_PROTO_RC5: {
        // Rebuild the 14-bit frame (MSB first): S1=1, S2 = inverted 7th command bit, T=0 (canonical toggle),
        // 5 address bits, low 6 command bits. Expand to 28 half-bits ('1' -> space,mark; '0' -> mark,space),
        // then emit what a transmitter actually puts on the wire — and what decode_rc5 reads back: drop S1's
        // leading space half-bit (carrier only starts at its mark) and a trailing space half-bit (a '0' last
        // bit's second half falls into idle), then run-length merge the rest into T / 2T durations from a mark.
        uint8_t addr = (uint8_t)(in->address & 0x1Fu);
        uint8_t cmd7 = (uint8_t)(in->command & 0x7Fu);
        uint8_t s2 = (uint8_t)((cmd7 & 0x40u) ? 0u : 1u);   // RC5X: a 7th command bit -> S2 = 0
        uint16_t val = (uint16_t)(((uint16_t)1u << 13)          // S1 = 1
                                  | ((uint16_t)s2 << 12)        // S2
                                  | ((uint16_t)addr << 6)       // A4..A0 (toggle T stays 0 at bit 11)
                                  | (uint16_t)(cmd7 & 0x3Fu));  // C5..C0
        uint8_t hb[28];
        for (size_t k = 0; k < 14u; k++) {
            uint8_t bit = (uint8_t)((val >> (13u - k)) & 1u);
            hb[2u * k] = (uint8_t)(bit ? 0 : 1);        // first half-bit
            hb[2u * k + 1u] = (uint8_t)(bit ? 1 : 0);   // second half-bit
        }
        if (cap < 27) {   // <= 27 durations (27 half-bits after dropping the implicit leading space)
            return false;
        }
        size_t hi = (hb[27] == 0u) ? 27u : 28u;   // exclusive end; skip the trailing space half-bit if present
        size_t i = 1u;                            // start past the implicit leading space (hb[0], always a space)
        while (i < hi) {
            uint8_t lvl = hb[i];
            size_t run = 1u;
            while (i + run < hi && hb[i + run] == lvl) {
                run++;
            }
            out[n++] = (uint16_t)(run * LXVEOS_RC5_T);   // run is 1 or 2 -> 889 or 1778 µs
            i += run;
        }
        break;
    }
    default:
        return false;   // UNKNOWN or an unsupported proto
    }
    *n_out = n;
    return true;
}

const char *lxveos_ir_proto_str(lxveos_ir_proto_t proto)
{
    switch (proto) {
    case LXVEOS_IR_PROTO_NEC:        return "NEC";
    case LXVEOS_IR_PROTO_NEC_REPEAT: return "NEC-repeat";
    case LXVEOS_IR_PROTO_SONY:       return "Sony";
    case LXVEOS_IR_PROTO_RC5:        return "RC5";
    default:                         return NULL;
    }
}
