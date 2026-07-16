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

// The ARM/SAFE transmit-posture banner + the arm state it currently shows. A repeating lv_timer re-reads the
// live arm state and repaints the banner when it changes, so arming/disarming from the CLI is reflected
// on-screen — the banner was once drawn once at boot and then went stale (a dangerous safety display). Touched
// only from the LVGL thread (the initial draw and the timer both run under the port lock).
static lv_obj_t *s_arm_lbl;
static lxveos_arm_state_t s_arm_shown;

// Paint the banner's text + colour for `st` and record it as shown. Caller must hold the LVGL port lock (true
// for both the initial draw and the lv_timer handler). Text + colour come from the one host-tested source.
static void arm_banner_apply(lxveos_arm_state_t st)
{
    if (s_arm_lbl == NULL) {
        return;
    }
    char banner[24];
    lxveos_gui_compose_arm_banner(banner, sizeof(banner), st);
    lv_label_set_text(s_arm_lbl, banner);
    lv_obj_set_style_text_color(s_arm_lbl, lv_color_hex(lxveos_gui_arm_banner_color(st)), 0);
    s_arm_shown = st;
}

// Repeating timer: repaint the banner only when the live arm state differs from what's on-screen. lv_timers
// run in the LVGL port task with the lock held, so this may repaint directly. The enum read is a single aligned
// word (the CLI task writes it) — a stale-by-one-tick posture indicator is the worst case, acceptable for a UI.
static void arm_banner_timer_cb(lv_timer_t *t)
{
    (void)t;
    lxveos_arm_state_t st = lxveos_arm_state();
    if (st != s_arm_shown) {
        arm_banner_apply(st);
    }
}

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
    // Created here, then repainted by arm_banner_timer_cb whenever the arm state changes (see above) so it
    // never goes stale after a CLI arm/disarm.
    s_arm_lbl = lv_label_create(scr);
    lv_obj_align(s_arm_lbl, LV_ALIGN_TOP_MID, 0, 5);
    arm_banner_apply(arm);   // initial text + colour from the one host-tested mapping

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

    // Keep the transmit-posture banner live: a 500ms timer repaints it when the arm state changes (armed /
    // disarmed from the CLI). Imperceptible for a safety indicator, negligible load, and it no-ops until the
    // state actually differs. Created under the port lock alongside the rest of the widget tree.
    lv_timer_create(arm_banner_timer_cb, 500, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL up: %dx%d, launcher + tappable capability list drawn (%u ops ready)", caps.width,
             caps.height, (unsigned)ready);
    return ESP_OK;
}
