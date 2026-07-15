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

#include "esp_lcd_types.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "lxveos_gui";

// Compose the capability menu as one formatted, grouped block: a category header (RECON / ATTACK /
// DEFENSE / …) whenever the category changes, then one line per op — a status glyph ([+] ready, [.]
// planned, [x] this board can't) + the op slug + a policy tag ((arm) for arm-gated offensive, (upstream)
// for the interference class LxveOS carries as control-surface only). Data comes straight from the op
// catalog so the on-device menu can never claim a feature the firmware doesn't actually have. Bounded.
static void compose_menu(char *buf, size_t cap)
{
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    size_t n = lxveos_ops_count();
    size_t off = 0;
    lxveos_opcat_t last = LXVEOS_OPCAT_COUNT;
    for (size_t i = 0; i < n; i++) {
        const lxveos_op_t *op = lxveos_ops_get(i);
        if (op == NULL) {
            continue;
        }
        if (op->category != last) {
            last = op->category;
            int w = snprintf(buf + off, cap - off, "%s%s\n", (i ? "\n" : ""), lxveos_opcat_name(last));
            if (w < 0 || (size_t)w >= cap - off) {
                break;
            }
            off += (size_t)w;
        }
        const char *g;
        switch (lxveos_op_status(op)) {
            case LXVEOS_OP_READY:      g = "[+]"; break;
            case LXVEOS_OP_PLANNED:    g = "[.]"; break;
            case LXVEOS_OP_ATTACHABLE: g = "[~]"; break;
            default:                   g = "[x]"; break;
        }
        lxveos_opclass_t k = lxveos_op_class(op);
        const char *tag = (k == LXVEOS_OPCLASS_OFFENSIVE)  ? " (arm)"
                        : (k == LXVEOS_OPCLASS_RESTRICTED) ? " (upstream)"
                        : "";
        int w = snprintf(buf + off, cap - off, " %s %s%s\n", g, op->slug, tag);
        if (w < 0 || (size_t)w >= cap - off) {
            break;
        }
        off += (size_t)w;
    }
}

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
    compose_menu(menu, sizeof(menu));

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
