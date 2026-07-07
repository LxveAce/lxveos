#pragma once
// LxveOS serial CLI + Cyber Controller bridge. On every board this is the headless control surface and
// the versioned UART protocol Cyber Controller talks to (detect/flash/telemetry). See build-architecture.md.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Print the banner + authorized-use notice and start the serial control surface.
esp_err_t lxveos_cli_start(void);

#ifdef __cplusplus
}
#endif
