#pragma once
// LxveOS IR (infrared) capture + replay — the `ir_recv` / `ir_send` catalog ops, a Flipper-class universal
// remote. Uses the ESP32 RMT peripheral: capture the raw symbol stream from an IR receiver on one GPIO,
// then re-emit that exact stream (38 kHz carrier) through an IR LED on another GPIO. Protocol-agnostic —
// it records and replays whatever was in the air (NEC, RC5, Sony SIRC, AC remotes, …) without needing to
// decode it.
//
// This is a benign RX/utility op (a universal remote), not an RF attack: it is single-signal capture +
// replay, NOT a code brute-forcer or an IR flood. It transmits only IR light on an operator-named GPIO.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Summary of one capture.
typedef struct {
    uint32_t symbols;    // raw RMT symbols captured (0 if nothing arrived before the timeout)
    bool     truncated;  // the signal was longer than the capture buffer and was cut off
} lxveos_ir_capture_info_t;

// Capture one IR transmission from an IR receiver module on `rx_gpio`. Brings an RMT RX channel up, waits
// up to `timeout_ms` (a sane default if 0) for a burst, stores the raw symbols internally (replacing any
// previous capture), and reports the count via *info (may be NULL). Returns ESP_OK on a capture,
// ESP_ERR_TIMEOUT if nothing arrived, ESP_ERR_INVALID_ARG for a bad GPIO, or an esp_err_t on RMT failure.
// Receive only — transmits nothing.
esp_err_t lxveos_ir_capture(int rx_gpio, uint32_t timeout_ms, lxveos_ir_capture_info_t *info);

// Replay the last captured IR signal through an IR LED on `tx_gpio` (modulated on a 38 kHz carrier). Returns
// ESP_ERR_INVALID_STATE if nothing has been captured, ESP_ERR_INVALID_ARG for a bad GPIO, or an esp_err_t
// on RMT failure. Emits exactly the captured symbols once.
esp_err_t lxveos_ir_replay(int tx_gpio);

// True if a capture is stored (available to replay). / Number of symbols in the stored capture.
bool lxveos_ir_have_capture(void);
uint32_t lxveos_ir_capture_symbols(void);

#ifdef __cplusplus
}
#endif
