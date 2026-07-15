// LxveOS on-device GUI — LVGL launcher. Brings the LVGL port up on the board's esp_lcd panel (resolved +
// initialised by lxveos_board) and draws the launcher: a branded header (title + board/op tally) and a
// scrollable capability menu built straight from the op catalog, grouped by category with per-op status
// and policy tags. Read-only display for now — touch/keypad activation (an LVGL indev via
// lvgl_port_add_touch + bsp_input_start) is the next increment.
//
// No-op on headless boards (null panel handle). UNVERIFIED on hardware; extra tunes the draw-buffer size,
// byte order (swap_bytes) and rotation on real glass. Widget surface is deliberately conservative
// (labels + one scroll container) so it stays green under our strict -Werror -Wextra toolchain.
#include "lxveos_gui.h"

#include "bsp/display.h"
#include "lxveos_board.h"
#include "lxveos_ops.h"
#include "lxveos_arm.h"
#include "lxveos_gui_menu.h"

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

    size_t ready = 0, planned = 0, attachable = 0, unavail = 0;
    lxveos_ops_tally(&ready, &planned, &attachable, &unavail);
    char hdr[48];
    // Header stays 3 numbers for width; attachable folds into the trailing "not-ready" count.
    snprintf(hdr, sizeof(hdr), "%s  ops %u/%u/%u", lxveos_ui_profile(),
             (unsigned)ready, (unsigned)planned, (unsigned)(attachable + unavail));
    static char menu[2600];
    lxveos_gui_compose_menu(menu, sizeof(menu));
    lxveos_arm_state_t arm = lxveos_arm_state();
    char banner[24];
    lxveos_gui_compose_arm_banner(banner, sizeof(banner), arm);

    // All LVGL object calls run under the port lock.
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);   // near-black brand backdrop

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LxveOS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x39FF14), 0);   // brand green
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 5);

    lv_obj_t *tally = lv_label_create(scr);
    lv_label_set_text(tally, hdr);
    lv_obj_set_style_text_color(tally, lv_color_hex(0x9A9A9A), 0);
    lv_obj_align(tally, LV_ALIGN_TOP_RIGHT, -6, 7);

    // ARM/SAFE banner — top-centre, coloured by state so the unit's transmit posture is unmissable:
    // red when ARMED (offensive TX permitted), amber while a two-factor arm is PENDING, brand-green SAFE.
    uint32_t arm_color = (arm == LXVEOS_ARM_ARMED)   ? 0xFF3030
                       : (arm == LXVEOS_ARM_PENDING) ? 0xFFB020
                                                     : 0x39FF14;
    lv_obj_t *arm_lbl = lv_label_create(scr);
    lv_label_set_text(arm_lbl, banner);
    lv_obj_set_style_text_color(arm_lbl, lv_color_hex(arm_color), 0);
    lv_obj_align(arm_lbl, LV_ALIGN_TOP_MID, 0, 5);

    // Scrollable capability menu (grouped by category) fills the area below the header.
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, caps.width, caps.height - 30);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0E0E0E), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);

    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, menu);
    lv_obj_set_width(lbl, caps.width - 20);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCFCFCF), 0);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL up: %dx%d, launcher + capability menu drawn (%u ops ready)", caps.width,
             caps.height, (unsigned)ready);
    return ESP_OK;
}
