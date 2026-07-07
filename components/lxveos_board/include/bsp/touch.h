#pragma once
// LxveOS board INPUT contract (esp-bsp-compatible subset). Each board's input[] from the manifest
// (touch / matrix-keyboard / buttons / trackball / encoder / IMU) is registered as an LVGL indev of
// the declared type; IMU feeds app-level events, not an indev. See build-architecture.md §4.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the board's input sources declared in the manifest. No-op on boards with no input.
esp_err_t bsp_input_start(void);

#ifdef __cplusplus
}
#endif
