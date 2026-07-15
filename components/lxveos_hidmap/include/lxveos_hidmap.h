#pragma once
// lxveos_hidmap — the US-layout USB-HID keystroke map used by the arm-gated BLE HID injection op
// ("BadBLE", lxveos_ble_hid.c): ASCII char -> (modifier, usage id) and named-key token -> usage id.
// Pulled out of the NimBLE driver so this pure, correctness-critical lookup (a wrong keycode types the
// wrong key) host-tests exhaustively off-target with no ESP-IDF/NimBLE toolchain (tests/host_c/test_hidmap.c).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Map one ASCII char to its US-layout HID modifier byte (*mod: 0 or MOD_LSHIFT 0x02) and usage id
// (*key). Handles a-z / A-Z / 0-9, space/Enter/Tab, and the US-keyboard punctuation set. Returns true
// when the char is typeable; false for anything unmapped (control bytes, non-ASCII), leaving *mod/*key 0.
// mod and key must be non-NULL.
bool ascii_to_hid(char c, uint8_t *mod, uint8_t *key);

// Map a DuckyScript-style named-key token to its HID usage id: ENTER/RETURN, TAB, ESC/ESCAPE, SPACE,
// BACKSPACE/BKSP, DELETE/DEL, arrow keys, HOME/END (case-insensitive), or a single letter. Returns 0
// for an unknown token or NULL. (A single letter maps like its lowercase HID usage.)
uint8_t named_key(const char *name);

#ifdef __cplusplus
}
#endif
