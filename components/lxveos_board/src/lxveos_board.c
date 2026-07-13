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
#if LXVEOS_DISP_HAS_PINS
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#endif
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

#if LXVEOS_DISP_HAS_PINS
// The M0 SPI display fleet (currently the CYD) drives the panel on HSPI / SPI2. A board that put its
// display on another host would key this off board_info.h; SPI2 is correct for the current fleet.
#define LXVEOS_LCD_SPI_HOST  SPI2_HOST

static esp_lcd_panel_io_handle_t s_panel_io;  // valid after a successful bring_up_panel_io()

// Bring up the display SPI bus + esp_lcd panel-IO handle from the generated GPIOs (LXVEOS_DISP_PIN_*).
// This is the transport half; creating the concrete panel (ST7789 built-in vs ILI9341 managed, per the
// resolved identity) + reset/init/backlight/LVGL is the next increment. The pins are community-verified
// (see cyd_boards.json display.pins _note) and compile-checked in CI — NOT yet run on this hardware.
static esp_err_t bring_up_panel_io(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = LXVEOS_DISP_PIN_SCLK,
        .mosi_io_num = LXVEOS_DISP_PIN_MOSI,
        .miso_io_num = LXVEOS_DISP_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LXVEOS_DISP_W * 80 * 2,  // ~80 lines of RGB565 per transfer
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LXVEOS_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "display SPI bus init");

    const esp_lcd_panel_io_spi_config_t iocfg = {
        .cs_gpio_num = LXVEOS_DISP_PIN_CS,
        .dc_gpio_num = LXVEOS_DISP_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LXVEOS_LCD_SPI_HOST,
                        &iocfg, &s_panel_io), TAG, "display panel-IO");
    ESP_LOGI(TAG, "panel-IO up: SPI2 sclk=%d mosi=%d cs=%d dc=%d bl=%d", LXVEOS_DISP_PIN_SCLK,
             LXVEOS_DISP_PIN_MOSI, LXVEOS_DISP_PIN_CS, LXVEOS_DISP_PIN_DC, LXVEOS_DISP_PIN_BL);
    return ESP_OK;
}
#endif  // LXVEOS_DISP_HAS_PINS

esp_err_t bsp_display_start(void)
{
    resolve_panel();
    ESP_LOGI(TAG, "display: %s %dx%d bus=%s backend=%s", s_panel, LXVEOS_DISP_W, LXVEOS_DISP_H,
             LXVEOS_DISP_BUS, LXVEOS_DISP_BACKEND);
#if LXVEOS_DISP_HAS_PINS
    ESP_RETURN_ON_ERROR(bring_up_panel_io(), TAG, "display bring-up");
    // TODO(M0): create the panel from the resolved identity — ST7789 via the built-in
    // esp_lcd_new_panel_st7789(), or ILI9341 via the managed espressif/esp_lcd_ili9341 component — then
    // reset/init, drive the GPIO21 backlight (active-HIGH), and hand the panel to esp_lvgl_port.
#else
    // This board's display GPIOs aren't in the manifest yet (pins=null). Source + verify them into
    // cyd_boards.json display.pins (datasheet/community, NOT fabricated) to unlock esp_lcd bring-up here.
#endif
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
    // Walk the board's input devices from board_info.h (generated from the manifest's input[]). M0 logs
    // each; TODO(M0): register each as an LVGL indev of the mapped type (pointer/keypad), and route
    // lvgl=none devices (e.g. an IMU) to app-level events instead of an indev.
#if LXVEOS_INPUT_COUNT > 0
#define X(cls, ctrl, bus, lvgl) \
    ESP_LOGI(TAG, "input: %-9s ctrl=%-10s bus=%-4s lvgl=%s", cls, (ctrl)[0] ? (ctrl) : "-", bus, lvgl);
    LXVEOS_INPUT_LIST(X)
#undef X
#else
    ESP_LOGI(TAG, "input: none (headless)");
#endif
    return ESP_OK;
}

esp_err_t lxveos_board_init(void)
{
    ESP_LOGI(TAG, "board '%s' (chip %s, ui '%s')", LXVEOS_BOARD_ID, LXVEOS_CHIP, LXVEOS_UI_PROFILE);
    ESP_ERROR_CHECK(bsp_display_start());
    ESP_ERROR_CHECK(bsp_input_start());
    return ESP_OK;
}
