#pragma once
// LxveOS board INPUT contract (esp-bsp-compatible subset). Each board's input[] from the manifest
// (touch / matrix-keyboard / buttons / trackball / encoder / IMU) is registered as an LVGL indev of
// the declared type; IMU feeds app-level events, not an indev.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the board's input sources declared in the manifest. No-op on boards with no input.
esp_err_t bsp_input_start(void);

// The board's touch controller as an opaque esp_lcd_touch handle, for the LVGL port to register as a pointer
// indev (lxveos_gui). NULL until bsp_input_start() brings it up, and on boards with no verified touch pinout
// (LXVEOS_TOUCH_HAS_PINS 0). Cast to esp_lcd_touch_handle_t.
void *bsp_touch_handle(void);

#ifdef __cplusplus
}
#endif
