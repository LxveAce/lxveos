#pragma once
// LxveOS on-device GUI — an LVGL launcher over the board's esp_lcd panel. The panel is already resolved
// and initialised by lxveos_board (bsp_display_start); this layer adds the LVGL port + the operator UI.
// Non-functional without a real panel: on headless boards, or before the panel is up, it is a no-op.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the LVGL port on the board's panel and draw the launcher. Returns ESP_OK (no-op) on headless
// boards / when no panel handle is available. Call once, after lxveos_board_init().
esp_err_t lxveos_gui_start(void);

#ifdef __cplusplus
}
#endif
