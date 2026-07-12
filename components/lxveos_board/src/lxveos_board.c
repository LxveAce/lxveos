// LxveOS board bring-up (M0 scaffold). Wires the esp-bsp-shaped structure and, on the CYD, the runtime
// panel-identity resolution (ILI9341 vs ST7789, cached to NVS). Real esp_lcd/LVGL panel + input bring-up
// is TODO(M0/M1); today this resolves + logs the panel and reports capabilities from board_info.h.
#include "lxveos_board.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "board_info.h"
#include "esp_log.h"

#if LXVEOS_HAS_DISPLAY
#include <stdio.h>
#include "nvs.h"
#endif

static const char *TAG = "lxveos_board";

const char *lxveos_board_id(void) { return LXVEOS_BOARD_ID; }
const char *lxveos_board_chip(void) { return LXVEOS_CHIP; }
const char *lxveos_ui_profile(void) { return LXVEOS_UI_PROFILE; }

#if LXVEOS_HAS_DISPLAY

#define LXVEOS_NVS_NS    "lxveos"
#define LXVEOS_NVS_PANEL "disp_panel"

static char s_panel[24];  // panel driver resolved for this boot; stable for the process

// Resolve the concrete panel driver for this boot. Fixed-panel boards use the generated
// LXVEOS_DISP_DRIVER. On the CYD (LXVEOS_DISP_RUNTIME_PROBE) the ILI9341 (1-USB) and ST7789 (2-USB)
// panels are pin-compatible, so identity is resolved once and cached to NVS; later boots read the cache
// and skip the probe. M0: the probe is a stub returning the build-time default. TODO(M1): drive the
// panel's RDDID / ID registers to tell ILI9341 from ST7789 before caching.
static void resolve_panel(void)
{
#if LXVEOS_DISP_RUNTIME_PROBE
    nvs_handle_t h;
    if (nvs_open(LXVEOS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "panel: NVS unavailable; using build default %s", LXVEOS_DISP_DRIVER);
        snprintf(s_panel, sizeof(s_panel), "%s", LXVEOS_DISP_DRIVER);
        return;
    }
    size_t len = sizeof(s_panel);
    if (nvs_get_str(h, LXVEOS_NVS_PANEL, s_panel, &len) == ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "panel: %s (cached)", s_panel);
        return;
    }
    // First boot on this unit: probe, then cache. M0 stub = the compile-time default panel.
    snprintf(s_panel, sizeof(s_panel), "%s", LXVEOS_DISP_DRIVER);
    esp_err_t w = nvs_set_str(h, LXVEOS_NVS_PANEL, s_panel);
    if (w == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "panel: %s (probed -> cached%s)", s_panel, w == ESP_OK ? "" : ", cache write failed");
#else
    snprintf(s_panel, sizeof(s_panel), "%s", LXVEOS_DISP_DRIVER);
    ESP_LOGI(TAG, "panel: %s (fixed)", s_panel);
#endif
}

const char *bsp_display_panel(void) { return s_panel; }

esp_err_t bsp_display_start(void)
{
    resolve_panel();
    ESP_LOGI(TAG, "display: %s %dx%d bus=%s backend=%s", s_panel, LXVEOS_DISP_W, LXVEOS_DISP_H,
             LXVEOS_DISP_BUS, LXVEOS_DISP_BACKEND);
    // TODO(M0): esp_lcd panel-IO + panel for the resolved driver; esp_lvgl_port bring-up.
    return ESP_OK;
}

esp_err_t bsp_display_get_caps(lxve_display_caps_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    out->width = LXVEOS_DISP_W;
    out->height = LXVEOS_DISP_H;
    out->bpp = 16;
    out->is_epaper = false;
    out->has_backlight = true;
    return ESP_OK;
}

esp_err_t bsp_display_backlight_set(uint8_t pct)
{
    (void)pct;
    return ESP_OK;  // TODO(M0): PWM backlight per board_info.h
}

#else  // headless — no panel

const char *bsp_display_panel(void) { return ""; }

esp_err_t bsp_display_start(void) { return ESP_OK; }

esp_err_t bsp_display_get_caps(lxve_display_caps_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (lxve_display_caps_t){0};
    return ESP_OK;
}

esp_err_t bsp_display_backlight_set(uint8_t pct)
{
    (void)pct;
    return ESP_OK;
}

#endif  // LXVEOS_HAS_DISPLAY

esp_err_t bsp_input_start(void)
{
    // TODO(M0): register manifest input[] as LVGL indevs (touch/keymatrix/buttons/encoder); IMU -> app events.
    return ESP_OK;
}

esp_err_t lxveos_board_init(void)
{
    ESP_LOGI(TAG, "board '%s' (chip %s, ui '%s')", LXVEOS_BOARD_ID, LXVEOS_CHIP, LXVEOS_UI_PROFILE);
    ESP_ERROR_CHECK(bsp_display_start());
    ESP_ERROR_CHECK(bsp_input_start());
    return ESP_OK;
}
