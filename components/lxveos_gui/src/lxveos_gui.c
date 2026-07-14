// LxveOS on-device GUI — LVGL launcher. Increment 3a: bring the LVGL port up on the board's esp_lcd
// panel and draw the launcher SHELL (branded title + a live status subtitle from board id / UI profile /
// op-catalog tally). The scrollable op-category menu + navigation land in 3b, once this proves the LVGL
// stack fetches, compiles under our strict toolchain, links, and initialises across the board matrix.
//
// Labels only here on purpose: the first LVGL compile is the risky part (managed lvgl + esp_lvgl_port
// under -Werror -Wextra), so 3a keeps the widget surface minimal. UNVERIFIED on hardware; extra tunes
// the draw buffer size, byte order (swap_bytes) and rotation on real glass.
#include "lxveos_gui.h"

#include "bsp/display.h"
#include "lxveos_board.h"
#include "lxveos_ops.h"

#include "esp_lcd_types.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "lxveos_gui";

esp_err_t lxveos_gui_start(void)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)bsp_display_panel_handle();
    esp_lcd_panel_io_handle_t io = (esp_lcd_panel_io_handle_t)bsp_display_io_handle();
    if (panel == NULL || io == NULL) {
        ESP_LOGI(TAG, "no panel on this board — GUI skipped (headless / CLI-only)");
        return ESP_OK;
    }

    lxve_display_caps_t caps;
    if (bsp_display_get_caps(&caps) != ESP_OK || caps.width == 0 || caps.height == 0) {
        ESP_LOGW(TAG, "display caps unavailable — GUI skipped");
        return ESP_OK;
    }

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    // Partial draw buffer (40 lines): the classic CYD has no PSRAM, so a full framebuffer won't fit.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = (uint32_t)caps.width * 40,
        .double_buffer = false,
        .hres = caps.width,
        .vres = caps.height,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,   // SPI RGB565 panels want the two colour bytes swapped (HW-tune)
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    size_t ready = 0, planned = 0, unavail = 0;
    lxveos_ops_tally(&ready, &planned, &unavail);
    char sub[96];
    snprintf(sub, sizeof(sub), "%s  ·  %s\n%u ready / %u planned / %u n-a",
             lxveos_board_id(), lxveos_ui_profile(),
             (unsigned)ready, (unsigned)planned, (unsigned)unavail);

    // All LVGL object calls run under the port lock.
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);   // near-black brand backdrop

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LxveOS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x39FF14), 0);   // brand green
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, sub);
    lv_obj_set_style_text_color(status, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 0);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL up: %dx%d, launcher shell drawn (%u ops ready)", caps.width, caps.height,
             (unsigned)ready);
    return ESP_OK;
}
