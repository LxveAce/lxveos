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
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
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

static esp_lcd_panel_io_handle_t s_panel_io;      // valid after a successful bring_up_panel_io()
static esp_lcd_panel_handle_t    s_panel_handle;  // concrete panel, valid after create_panel()
static uint8_t s_probe_d3[3];                     // RDID4 (0xD3) read at bring-up: 00 93 41 => ILI9341

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

    // Probe the panel over the IO handle: read its ID registers (RDDID 0x04, and RDID4 0xD3 which
    // returns 00 93 41 on an ILI9341). A real panel on these pins answers with a meaningful ID; an
    // unconnected bus reads all-0x00 / all-0xFF. This is both the flagship ILI9341-vs-ST7789 probe
    // seed and a positive "is this board physically the CYD?" hardware check over serial.
    uint8_t id04[3] = {0}, idd3[3] = {0};
    esp_err_t r04 = esp_lcd_panel_io_rx_param(s_panel_io, 0x04, id04, sizeof(id04));
    esp_err_t rd3 = esp_lcd_panel_io_rx_param(s_panel_io, 0xD3, idd3, sizeof(idd3));
    ESP_LOGI(TAG, "panel probe: RDDID(0x04)=%02x %02x %02x [%s]  RDID4(0xD3)=%02x %02x %02x [%s]",
             id04[0], id04[1], id04[2], esp_err_to_name(r04),
             idd3[0], idd3[1], idd3[2], esp_err_to_name(rd3));
    s_probe_d3[0] = idd3[0];  // kept for create_panel()'s ILI9341 (00 93 41) vs ST7789 decision
    s_probe_d3[1] = idd3[1];
    s_probe_d3[2] = idd3[2];

    // Backlight on (GPIO21, active-HIGH on the CYD). Simple push-pull drive for this probe; LEDC PWM
    // dimming arrives with the panel driver. A lit backlight is the visible "this is the CYD" signal.
#if LXVEOS_DISP_PIN_BL >= 0
    const gpio_config_t blcfg = {
        .pin_bit_mask = 1ULL << LXVEOS_DISP_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&blcfg), TAG, "backlight gpio");
    gpio_set_level(LXVEOS_DISP_PIN_BL, 1);
    ESP_LOGI(TAG, "backlight GPIO%d -> HIGH", LXVEOS_DISP_PIN_BL);
#endif
    return ESP_OK;
}

// Create the concrete esp_lcd panel on the panel-IO handle, initialise it, and paint a solid
// proof-of-life fill. The panel driver is chosen from the RDID4 (0xD3) probe: a 1-USB CYD reads
// 00 93 41 => ILI9341 (the managed espressif/esp_lcd_ili9341 driver); a 2-USB CYD => ST7789 (built into
// esp_lcd). Both share the esp_lcd_panel_dev_config_t + panel-ops interface. reset_gpio_num = -1 is valid
// (RST tied to EN on the CYD). A lit, green-filled screen is the visible "the panel is really up" signal.
// UNVERIFIED on hardware; extra confirms which variant + tunes byte order/rotation on real glass.
static esp_err_t create_panel(void)
{
    const bool is_ili9341 = (s_probe_d3[0] == 0x00 && s_probe_d3[1] == 0x93 && s_probe_d3[2] == 0x41);
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = LXVEOS_DISP_PIN_RST,        // -1 == RST tied to EN (no dedicated reset line)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // CYD panels are BGR
        .bits_per_pixel = 16,
    };
    if (is_ili9341) {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(s_panel_io, &pcfg, &s_panel_handle), TAG, "new ili9341");
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &pcfg, &s_panel_handle), TAG, "new st7789");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init");
    esp_lcd_panel_invert_color(s_panel_handle, !is_ili9341);   // ST7789 needs inversion; ILI9341 does not
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    // Proof-of-life: paint the panel LxveAce green in DRAM-friendly 16-line horizontal bands (no full
    // framebuffer — the classic CYD has no PSRAM). Every band holds identical pixels, so reusing the one
    // static buffer across queued draws is safe. Byte order / exact hue are HW-tune once on real glass.
    static uint16_t band[LXVEOS_DISP_W * 16];
    const uint16_t green = 0x3FE2;   // #39FF14 in RGB565
    for (int i = 0; i < LXVEOS_DISP_W * 16; i++) {
        band[i] = green;
    }
    for (int y = 0; y < LXVEOS_DISP_H; y += 16) {
        int h = (y + 16 <= LXVEOS_DISP_H) ? 16 : (LXVEOS_DISP_H - y);
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, LXVEOS_DISP_W, y + h, band);
    }
    ESP_LOGI(TAG, "panel up: %s %dx%d initialised + filled (proof-of-life)",
             is_ili9341 ? "ILI9341" : "ST7789", LXVEOS_DISP_W, LXVEOS_DISP_H);
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
    ESP_RETURN_ON_ERROR(create_panel(), TAG, "panel create");
    // TODO(M1-C next): add LEDC PWM backlight dimming, then hand s_panel_handle/s_panel_io to
    // esp_lvgl_port and build the LVGL launcher (lxveos_gui).
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

// Opaque esp_lcd handles for the LVGL port layer (lxveos_gui). NULL until create_panel() runs, and on
// display boards whose GPIOs aren't in the manifest yet. Cast to esp_lcd_panel_handle_t / _io_handle_t.
void *bsp_display_panel_handle(void)
{
#if LXVEOS_DISP_HAS_PINS
    return s_panel_handle;
#else
    return NULL;
#endif
}
void *bsp_display_io_handle(void)
{
#if LXVEOS_DISP_HAS_PINS
    return s_panel_io;
#else
    return NULL;
#endif
}

#else  // headless — no panel

const char *bsp_display_panel(void) { return ""; }
void *bsp_display_panel_handle(void) { return NULL; }
void *bsp_display_io_handle(void) { return NULL; }

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
