// LxveOS on-device GUI — LVGL launcher. Brings the LVGL port up on the board's esp_lcd panel (resolved +
// initialised by lxveos_board) and draws the launcher: a branded header (title + board/op tally) and a
// tappable capability list built straight from the op catalog, grouped by category with per-op status and
// policy tags. Tapping an op opens a detail card (compose_detail) with a close button — the interactive
// launcher. Tap-to-RUN (op dispatch) is the next increment and is HW-gated: nothing is authored on-air here.
//
// No-op on headless boards (null panel handle). UNVERIFIED on hardware; extra tunes the draw-buffer size,
// byte order (swap_bytes), rotation, and the list/detail layout + touch calibration on real glass. The widget
// surface stays conservative (core lv_obj containers + lv_label only — no optional lv_list/lv_button) so it
// holds green under our strict -Werror -Wextra toolchain and doesn't depend on non-default LVGL widget flags.
#include "lxveos_gui.h"

#include "bsp/display.h"
#include "bsp/touch.h"
#include "lxveos_board.h"
#include "lxveos_ops.h"
#include "lxveos_arm.h"
#include "lxveos_gui_menu.h"

#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>

static const char *TAG = "lxveos_gui";

// The op-detail card currently open (NULL when none). One at a time: tapping another op replaces it. All
// access is from LVGL event callbacks, which run single-threaded under the LVGL port lock.
static lv_obj_t *s_detail;

// Close (delete) the open detail card. Wired to its "close" button.
static void detail_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail != NULL) {
        lv_obj_delete(s_detail);
        s_detail = NULL;
    }
}

// Tap handler for a capability-list row: open (or replace) the detail card for that op. The op's catalog
// index rides in the event user-data. This shows read-only detail text (compose_detail) — it does NOT run
// the op; tap-to-dispatch is a later HW-gated increment, so nothing is transmitted here.
static void op_row_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    const lxveos_op_t *op = lxveos_ops_get(idx);
    static char detail[320];
    lxveos_gui_compose_detail(detail, sizeof(detail), op);

    if (s_detail != NULL) {
        lv_obj_delete(s_detail);
    }
    lv_obj_t *scr = lv_screen_active();
    s_detail = lv_obj_create(scr);
    lv_obj_set_size(s_detail, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_detail, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 8, 0);

    lv_obj_t *text = lv_label_create(s_detail);
    lv_label_set_text(text, detail);
    lv_obj_set_width(text, lv_pct(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(text, lv_color_hex(0xCFCFCF), 0);
    lv_obj_align(text, LV_ALIGN_TOP_LEFT, 0, 2);

    // "close" control — a clickable lv_obj (core widget), so we don't depend on the optional lv_button build flag.
    lv_obj_t *close = lv_obj_create(s_detail);
    lv_obj_set_size(close, 70, 32);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close, detail_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, "close");
    lv_obj_center(close_lbl);
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

    // Register the touch panel (if the board brought one up in bsp_input_start) as an LVGL pointer indev, so
    // the launcher is tappable/scrollable. Non-fatal: a display-only board (or a failed indev) still shows the
    // read-only launcher. Coordinate calibration is a HW-tune, done on real glass.
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)bsp_touch_handle();
    if (touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed — launcher stays read-only");
        } else {
            ESP_LOGI(TAG, "touch indev registered (XPT2046 pointer)");
        }
    }

    size_t ready = 0, planned = 0, attachable = 0, unavail = 0;
    lxveos_ops_tally(&ready, &planned, &attachable, &unavail);
    char hdr[48];
    // Header stays 3 numbers for width; attachable folds into the trailing "not-ready" count.
    snprintf(hdr, sizeof(hdr), "%s  ops %u/%u/%u", lxveos_ui_profile(),
             (unsigned)ready, (unsigned)planned, (unsigned)(attachable + unavail));
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

    // Tappable capability list (grouped by category) fills the area below the header. Built from CORE widgets
    // only — a scrollable lv_obj with flex-column layout, one clickable lv_obj row per op (+ an lv_label child)
    // — so it never leans on the optional lv_list / lv_button build flags (LV_USE_LIST / LV_USE_BUTTON), which
    // this project doesn't force on. Tapping a row opens that op's detail card (op_row_cb). The classic CYD has
    // no PSRAM, so this ~35-row list is the RAM-heaviest surface: a HW-tune point (row count / LVGL heap).
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, caps.width, caps.height - 30);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0E0E0E), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lxveos_opcat_t last_cat = LXVEOS_OPCAT_COUNT;
    for (size_t i = 0; i < lxveos_ops_count(); i++) {
        const lxveos_op_t *op = lxveos_ops_get(i);
        if (op == NULL) {
            continue;
        }
        if (op->category != last_cat) {            // non-clickable category header (a plain flex-item label)
            last_cat = op->category;
            lv_obj_t *cat = lv_label_create(list);
            lv_label_set_text(cat, lxveos_opcat_name(last_cat));
            lv_obj_set_style_text_color(cat, lv_color_hex(0x39FF14), 0);
            lv_obj_set_style_pad_top(cat, 4, 0);
        }
        char row_txt[64];
        lxveos_gui_compose_op_label(row_txt, sizeof(row_txt), op);
        lv_obj_t *row = lv_obj_create(list);       // clickable core-widget row (no lv_button dependency)
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 5, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);   // the row shouldn't eat the tap as an inner scroll
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, op_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *row_lbl = lv_label_create(row);
        lv_label_set_text(row_lbl, row_txt);
        lv_obj_set_style_text_color(row_lbl, lv_color_hex(0xCFCFCF), 0);
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL up: %dx%d, launcher + tappable capability list drawn (%u ops ready)", caps.width,
             caps.height, (unsigned)ready);
    return ESP_OK;
}
