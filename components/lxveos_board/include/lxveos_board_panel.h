#pragma once
// Pure, host-testable panel helpers for the board layer (no ESP-IDF deps) — split out of lxveos_board.c so
// the runtime driver-identity decision can be unit-tested off-target (tests/host_c/test_board_panel.c).
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Map a classic-CYD RDID4 (register 0xD3) probe read to its concrete panel driver name. A 1-USB CYD reports
// 00 93 41 -> "ILI9341" (the managed espressif/esp_lcd_ili9341 driver); anything else is a 2-USB CYD ->
// "ST7789" (built into esp_lcd). Never returns NULL. `d3` must point to at least 3 bytes.
const char *lxveos_panel_from_probe_d3(const uint8_t d3[3]);

#ifdef __cplusplus
}
#endif
