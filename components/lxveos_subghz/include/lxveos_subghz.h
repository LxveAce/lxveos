#pragma once
// LxveOS sub-GHz (CC1101) — the `subghz_scan` recon op (increment 1: bring-up + presence + RSSI spectrum
// sense). The TI CC1101 is an external SPI transceiver on an add-on module (315/433/868/915 MHz ISM bands),
// the classic Flipper/ESP32-DIV sub-GHz radio. This increment brings the SPI link up on operator-supplied
// pins, resets + identifies the chip (PARTNUM/VERSION registers — the first thing to confirm on hardware),
// and samples RSSI at a chosen frequency so an operator can find active sub-GHz signals. RECEIVE ONLY —
// it puts nothing on-air. Capture + single-signal replay (the `subghz_replay` op) is a later increment and
// will be arm-gated.
//
// Pins are operator-supplied (the module is an add-on; wiring differs per board), so this is not cap-gated.
// It uses SPI3 (VSPI on ESP32) to avoid the display's SPI2/HSPI bus.
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring the SPI link up on the given GPIOs, reset the CC1101, load a base 433 MHz OOK-RX register profile,
// and read its identity. Stores the SPI handle for subsequent calls (call lxveos_subghz_end() to release).
// Returns ESP_OK if the SPI transaction succeeded (check lxveos_subghz_present() for a valid chip), an
// esp_err_t on SPI init failure, or ESP_ERR_INVALID_STATE if already begun. Transmits nothing.
esp_err_t lxveos_subghz_begin(int sclk, int miso, int mosi, int cs);

// Release the SPI bus/device (safe to call when not begun).
esp_err_t lxveos_subghz_end(void);

// True if a CC1101 answered with a plausible VERSION register value after the last begin().
bool lxveos_subghz_present(void);
// The raw PARTNUM (reg 0x30) and VERSION (reg 0x31) bytes read at begin() — CC1101 is typically 0x00/0x14.
uint8_t lxveos_subghz_partnum(void);
uint8_t lxveos_subghz_version(void);

// Tune to `mhz` (300-928 MHz), enter RX, settle, and sample the RSSI register, converting it to dBm into
// *rssi_dbm. A quick "is anything transmitting here?" spectrum probe. Returns ESP_OK, ESP_ERR_INVALID_STATE
// if not begun, or ESP_ERR_INVALID_ARG for an out-of-band frequency. Receive only.
esp_err_t lxveos_subghz_rssi(float mhz, int8_t *rssi_dbm);

// ── Increment 2: OOK capture + replay (the subghz_replay op) ─────────────────────────────────────────
// Capture a raw sub-GHz OOK transmission at `mhz`: puts the CC1101 in async-serial RX (GDO0 outputs the
// demodulated bitstream) and records the GDO0 edge timing via RMT on `gdo0_gpio` for up to `timeout_ms`.
// Stores the symbols internally (replacing any prior capture). Protocol-agnostic — records whatever OOK
// signal is in the air (garage/gate/sensor remotes). Returns ESP_OK, ESP_ERR_TIMEOUT if nothing arrived,
// ESP_ERR_INVALID_STATE if not begun, ESP_ERR_INVALID_ARG for a bad gpio/frequency. Receive only.
esp_err_t lxveos_subghz_capture(int gdo0_gpio, float mhz, uint32_t timeout_ms, uint32_t *symbols);

// Replay the last captured OOK signal: puts the CC1101 in async-serial TX at the captured frequency and
// re-emits the stored edge timing via RMT on `gdo0_gpio` (the CC1101 does the RF modulation; no RMT
// carrier). This is an OFFENSIVE-TX op — gated on the arm framework (lxveos_arm_can_emit() must be true).
// Returns ESP_ERR_NOT_ALLOWED if not armed / TX compiled out, ESP_ERR_INVALID_STATE if nothing captured or
// not begun, ESP_ERR_INVALID_ARG for a bad gpio, or an esp_err_t. Emits the one captured signal once.
esp_err_t lxveos_subghz_replay(int gdo0_gpio);

// True if an OOK capture is stored. / Number of symbols in the stored capture.
bool lxveos_subghz_have_capture(void);
uint32_t lxveos_subghz_capture_symbols(void);

// Copy the stored OOK capture out as its raw pulse-duration train (microseconds, alternating HIGH,LOW,…)
// into `out`, up to `cap` entries; returns the number of durations written. For inspection and OOK decode
// (feed the result to lxveos_ook_decode). Receive-only — reads the stored capture, transmits nothing.
uint32_t lxveos_subghz_capture_durations(uint16_t *out, uint32_t cap);

#ifdef __cplusplus
}
#endif
