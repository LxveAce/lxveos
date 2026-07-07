#pragma once
// LxveOS board DISPLAY contract — an esp-bsp-compatible subset. Every board (upstream esp-bsp,
// first-party CYD/Cardputer BSP, or the future LxveNode) is interchangeable behind this one header.
// Backend (esp_lcd / LovyanGFX / e-paper) is chosen from the generated board_info.h at build time.
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    uint8_t bpp;
    bool is_epaper;
    bool has_backlight;
} lxve_display_caps_t;

// Bring up this board's panel. On the classic CYD this runs the runtime ILI9341/ST7789 probe and
// caches the resolved panel to NVS. No-op (returns ESP_OK) on headless boards.
esp_err_t bsp_display_start(void);
esp_err_t bsp_display_get_caps(lxve_display_caps_t *out);
esp_err_t bsp_display_backlight_set(uint8_t pct);

#ifdef __cplusplus
}
#endif
