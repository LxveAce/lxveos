#pragma once
// LxveOS nRF24 (nRF24L01+) — the `nrf24_scan` recon op (increment 1: 2.4 GHz channel-activity scan). The
// Nordic nRF24L01+ is a cheap 2.4 GHz SPI transceiver (the Bastille "MouseJack" radio, also on ESP32-DIV /
// Marauder add-ons). This increment brings the SPI link up on operator-supplied pins, identifies the chip,
// and does the classic RPD (Received Power Detector) sweep across all 126 channels — a poor-man's 2.4 GHz
// spectrum analyzer that flags which channels have energy above ~-64 dBm (Wi-Fi, BLE, nRF peripherals, …).
// RECEIVE ONLY — it puts nothing on-air. MouseJack keystroke injection (`nrf24_mousejack`) is a later,
// arm-gated increment.
//
// Pins are operator-supplied (add-on module): SPI SCK/MISO/MOSI, CSN (SPI chip-select) and CE (a plain GPIO
// that gates RX/TX). Uses SPI3/VSPI to stay off the display's SPI2. Not cap-gated.
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXVEOS_NRF24_CHANNELS 126   // nRF24 channels 0..125 (2400..2525 MHz, 1 MHz spacing)

// Bring up SPI3 + the CE GPIO on the given pins, configure the nRF24 as a primary receiver, and probe for
// the chip (a register write/read-back). Stores handles for later calls. Returns ESP_OK on SPI success
// (check lxveos_nrf24_present()), an esp_err_t on init failure, or ESP_ERR_INVALID_STATE if already begun.
esp_err_t lxveos_nrf24_begin(int sck, int miso, int mosi, int csn, int ce);

// Release the SPI device/bus + CE GPIO (safe when not begun).
esp_err_t lxveos_nrf24_end(void);

// True if the register write/read-back at begin() matched (a chip is responding on the bus).
bool lxveos_nrf24_present(void);

// Sweep every channel `sweeps` times (clamped to a sane range), sampling the RPD flag on each, and write the
// per-channel hit count into counts[LXVEOS_NRF24_CHANNELS] (higher = busier). Returns ESP_OK, or
// ESP_ERR_INVALID_STATE if not begun, ESP_ERR_INVALID_ARG for a NULL buffer. Receive only.
esp_err_t lxveos_nrf24_scan(uint8_t *counts, uint16_t sweeps);

#ifdef __cplusplus
}
#endif
