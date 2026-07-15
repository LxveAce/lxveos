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

// ── nRF24 Logitech Unifying ───────────────────────────────────────────────────────────────────────────
// Unifying frames carry a trailing checksum byte chosen so the whole frame sums to 0 mod 256.
// Return the checksum byte for a frame whose first n-1 bytes are set (the last byte is the slot it fills).
uint8_t lxveos_unifying_checksum(const uint8_t *frame, size_t n);
// True when frame[0..n-1] already sums to 0 mod 256 (i.e. the trailing checksum byte is correct).
bool lxveos_unifying_checksum_ok(const uint8_t *frame, size_t n);
