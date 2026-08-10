#pragma once
// Pure, dependency-free (libc-only) arithmetic shared by the external-radio drivers: CC1101 sub-GHz,
// PN532 NFC, and nRF24 Mousejack. Extracted here so the frequency-word / RSSI / frame-checksum math has one
// source of truth the on-target drivers AND the host unit tests (tests/host_c/test_radiomath.c) both read —
// no ESP-IDF SPI/I2C layer needed to exercise it. Nothing here touches a radio; it only computes register
// and frame bytes. The drivers stay `implemented=false` until HW-validated — these functions being correct
// is necessary but not sufficient for that (CI proves the math, never the RF).
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ── CC1101 sub-GHz ────────────────────────────────────────────────────────────────────────────────────
// Frequency programming word: FREQ = f / (fXOSC / 2^16), with the standard 26 MHz crystal. The driver
// splits the returned 24-bit value across FREQ2/FREQ1/FREQ0. `mhz` is the carrier in MHz (e.g. 433.92).
uint32_t lxveos_cc1101_freq_to_word(float mhz);
// TI RSSI conversion: rssi_dBm = rssi_dec/2 - offset, where rssi_dec is the raw byte read as two's
// complement and offset is 74 dB (the 433 MHz figure). Clamped to the int8_t range the driver returns.
int8_t lxveos_cc1101_rssi_to_dbm(uint8_t raw);

// ── PN532 NFC normal-mode frame ───────────────────────────────────────────────────────────────────────
// A host->reader frame is: 00 00 FF LEN LCS TFI cmd... DCS 00, where LEN = len(TFI..cmd), LCS = -LEN and
// DCS = -(TFI + sum(cmd)), both two's-complement so the checksummed run sums to 0 mod 256.
// Length checksum for a given LEN byte.
uint8_t lxveos_pn532_lcs(uint8_t len);
// Data checksum over the TFI byte plus `plen` payload bytes.
uint8_t lxveos_pn532_dcs(uint8_t tfi, const uint8_t *payload, uint8_t plen);
// Build a command frame into `out`. `tfi` is normally 0xD4 (host). Returns the total frame length written
// (clen + 8), or 0 if it would not fit in `out_cap` or an argument is NULL.
size_t lxveos_pn532_build_frame(uint8_t tfi, const uint8_t *cmd, uint8_t clen, uint8_t *out, size_t out_cap);
// Validate a received frame's preamble, LEN/LCS pair, DCS and postamble. `n` = readable bytes in `frame`.
bool lxveos_pn532_frame_valid(const uint8_t *frame, size_t n);
// MIFARE Classic block-0 BCC (UID check byte) = XOR of the four UID bytes.
uint8_t lxveos_mifare_bcc4(const uint8_t uid[4]);
// MIFARE Classic Gen2 "magic card" block 0 (16 bytes) = UID(4) + BCC + SAK 0x08 + ATQA 0x0004 +
// 8-byte manufacturer pad. `bcc` is computed via lxveos_mifare_bcc4. Fills the 16-byte block payload
// only (the PN532 write-command wrapper is the caller's). No card is written here — byte layout only.
void lxveos_mifare_build_block0(const uint8_t uid[4], uint8_t out[16]);

// ── nRF24 Logitech Unifying ───────────────────────────────────────────────────────────────────────────
// Unifying frames carry a trailing checksum byte chosen so the whole frame sums to 0 mod 256.
// Return the checksum byte for a frame whose first n-1 bytes are set (the last byte is the slot it fills).
uint8_t lxveos_unifying_checksum(const uint8_t *frame, size_t n);
// True when frame[0..n-1] already sums to 0 mod 256 (i.e. the trailing checksum byte is correct).
bool lxveos_unifying_checksum_ok(const uint8_t *frame, size_t n);
// Logitech Unifying unencrypted keyboard frame (10 bytes): dev-idx 0x00, report-type 0xC1, modifier,
// keycode, 5 zero pad, trailing checksum (via lxveos_unifying_checksum). `press` selects the keypress
// frame (mod+key) vs the release frame (both zero). Fills out[10] — byte layout only, no radio TX.
void lxveos_unifying_build_kbd_frame(uint8_t mod, uint8_t key, bool press, uint8_t out[10]);

// ── Sub-GHz OOK PWM decode (EV1527 / PT2262 remotes) ──────────────────────────────────────────────────
// Decode a captured OOK pulse train (durations in microseconds, alternating HIGH,LOW,HIGH,LOW…, the line
// idling LOW so durations[0] is a HIGH) into '0'/'1' ASCII bits. Uses the PWM scheme those cheap 315/433 MHz
// remotes share: the base time Te is the shortest pulse; a data bit is a HIGH+LOW pair — short-high+long-low
// is '0', long-high+short-low is '1' — and frames are separated by a long (~31 Te) LOW sync gap. A leading
// sync gap is skipped; decoding stops at the next gap, at an ambiguous pair, or when `bits_cap` is reached.
// Writes up to `bits_cap` chars (no NUL) to `bits` and returns the number of bits decoded (0 if the train is
// too short / no clean Te / all noise). Pure: no radio, no allocation.
size_t lxveos_ook_decode(const uint16_t *durations, size_t n, char *bits, size_t bits_cap);

// A decoded fixed/learning-code OOK remote codeword. The common 315/433 MHz remotes (EV1527, and the
// EV1527-compatible bit framing of PT2262-style encoders) send a 24-bit word: the top 20 bits are the
// remote's address and the low 4 are the button/data nibble. (True PT2262 tri-state pin states are NOT
// resolved here — this is the bit-level frame lxveos_ook_decode recovers, split by the 20+4 convention.)
typedef struct {
    uint32_t address;  // 20-bit remote address (top 20 bits of the 24-bit word)
    uint8_t button;    // 4-bit button/data nibble (low 4 bits)
    uint8_t nbits;     // number of bits parsed (24 for a standard frame)
    bool valid;        // true when `bits` was a well-formed 24-bit binary codeword
} lxveos_ook_codeword_t;

// Parse the '0'/'1' bit-string lxveos_ook_decode writes into a 24-bit OOK codeword (20-bit address + 4-bit
// button). `bits` holds `nbits` ASCII '0'/'1' chars (no NUL required). Returns true and fills *out for a
// valid 24-bit binary frame; returns false (and sets out->valid=false) for a wrong length, a non-binary
// char, or a NULL argument. Pure: no radio, no allocation.
bool lxveos_ook_codeword(const char *bits, size_t nbits, lxveos_ook_codeword_t *out);
