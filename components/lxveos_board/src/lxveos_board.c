// LxveOS board bring-up (M0 scaffold). Real esp_lcd/LVGL panel + input bring-up is TODO(M0);
// this wires the structure and logs the selected board's capabilities from the generated board_info.h.
#include "lxveos_board.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "board_info.h"
#include "esp_log.h"

static const char *TAG = "lxveos_board";

const char *lxveos_board_id(void) { return LXVEOS_BOARD_ID; }
const char *lxveos_ui_profile(void) { return LXVEOS_UI_PROFILE; }

esp_err_t bsp_display_start(void)
{
#if LXVEOS_HAS_DISPLAY
    ESP_LOGI(TAG, "display: %s %dx%d bus=%s backend=%s%s",
             LXVEOS_DISP_DRIVER, LXVEOS_DISP_W, LXVEOS_DISP_H, LXVEOS_DISP_BUS, LXVEOS_DISP_BACKEND,
             LXVEOS_DISP_RUNTIME_PROBE ? " [runtime ILI9341/ST7789 probe -> NVS]" : "");
    // TODO(M0): esp_lcd panel-IO + panel per board_info.h; runtime panel probe for CYD; esp_lvgl_port bring-up.
    return ESP_OK;
#else
    return ESP_OK; // headless
#endif
}

esp_err_t bsp_display_get_caps(lxve_display_caps_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
#if LXVEOS_HAS_DISPLAY
    out->width = LXVEOS_DISP_W;
    out->height = LXVEOS_DISP_H;
    out->bpp = 16;
    out->is_epaper = false;
    out->has_backlight = true;
#else
    *out = (lxve_display_caps_t){0};
#endif
    return ESP_OK;
}

esp_err_t bsp_display_backlight_set(uint8_t pct)
{
    (void)pct;
    return ESP_OK; // TODO(M0): PWM backlight per board_info.h
}

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
