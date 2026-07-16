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

// ── Protocol decode (NEC / Sony SIRC) ───────────────────────────────────────────────────────────────────
// The capture/replay above is protocol-agnostic (record + re-emit raw symbols). This adds an OPTIONAL pure
// decoder that recognizes the two dominant consumer IR protocols and pulls out their address/command, so a
// capture can be reported as "NEC addr=0x04 cmd=0x08" instead of just a symbol count. RC5/RC6 (Manchester)
// are not decoded yet — an undecodable capture still records + replays fine.
typedef enum {
    LXVEOS_IR_PROTO_UNKNOWN = 0,
    LXVEOS_IR_PROTO_NEC,         // NEC (8-bit address, or 16-bit "extended" when the address byte isn't inverted)
    LXVEOS_IR_PROTO_NEC_REPEAT,  // NEC repeat code (a held-down button), no payload
    LXVEOS_IR_PROTO_SONY,        // Sony SIRC (12 / 15 / 20-bit)
} lxveos_ir_proto_t;

typedef struct {
    lxveos_ir_proto_t proto;
    uint16_t address;   // device/address field (NEC 8 or 16-bit; Sony the bits above the 7-bit command)
    uint16_t command;   // command field (NEC 8-bit; Sony low 7 bits)
    uint8_t  bits;      // payload bit count (NEC 32; Sony 12/15/20; 0 for a repeat code)
    bool     addr_ext;  // NEC only: the address byte was not its own inverse -> 16-bit extended addressing
} lxveos_ir_decoded_t;

// Decode a captured IR mark/space duration train (microseconds; durations[0] is a MARK — carrier on — then
// alternating SPACE, MARK, …) into a protocol + address/command. Recognizes NEC (and its repeat code) and
// Sony SIRC within timing tolerance. Returns true and fills *out on a confident decode (NEC also requires the
// command's inverted-byte integrity check to pass); returns false and leaves *out = UNKNOWN otherwise. Pure:
// no radio, no allocation — this is the host-tested core (tests/host_c/test_ir_decode.c).
bool lxveos_ir_decode(const uint16_t *durations, size_t n, lxveos_ir_decoded_t *out);

// Short protocol label ("NEC"/"NEC-repeat"/"Sony"), or NULL for LXVEOS_IR_PROTO_UNKNOWN.
const char *lxveos_ir_proto_str(lxveos_ir_proto_t proto);

// Decode the LAST stored capture (flattens the retained RMT symbols to a duration train and runs
// lxveos_ir_decode). Returns false with *out=UNKNOWN if nothing is stored or the signal isn't recognized.
bool lxveos_ir_decode_last(lxveos_ir_decoded_t *out);

#ifdef __cplusplus
}
#endif
