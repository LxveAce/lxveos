#pragma once
// Internal text-composition helpers for the LxveOS on-device GUI, split out of lxveos_gui.c so the pure
// string building (no LVGL, no esp_lcd) can be host-unit-tested (tests/host_c/test_gui_menu.c). lxveos_gui.c
// wraps the results in LVGL labels.
#include <stddef.h>
#include <stdint.h>

#include "lxveos_arm.h"
#include "lxveos_ops.h"

// Compose the one-line list-button label for a single op into `buf` (bounded by `cap`): the status glyph
// ([+]/[.]/[~]/[x]), the slug, and the policy tag ((arm)/(upstream)) — the per-op rendering the GUI puts on
// each tappable row (B12d interactive launcher). A NULL op yields a short placeholder.
void lxveos_gui_compose_op_label(char *buf, size_t cap, const lxveos_op_t *op);

// Compose the ARM/SAFE banner text for the given arm state into `buf` (bounded by `cap`). Pure mapping of the
// two-factor arm state to a short, unambiguous banner the header shows (coloured by lxveos_gui.c).
void lxveos_gui_compose_arm_banner(char *buf, size_t cap, lxveos_arm_state_t state);

// The colour (0xRRGGBB, wrapped by the GUI in lv_color_hex) for the ARM/SAFE banner in a given arm state: red
// ARMED / amber PENDING / brand-green SAFE (unknown -> green, never a false "live" red). Kept beside
// compose_arm_banner so the banner text and colour never drift, and shared by the initial draw + refresh timer.
uint32_t lxveos_gui_arm_banner_color(lxveos_arm_state_t state);

// Compose the multi-line detail card for one op into `buf` (bounded by `cap`): title, slug, category +
// runtime status, the capability it needs, its upstream inspiration, and — for offensive/restricted ops — a
// policy line spelling out the arm/upstream-TX requirement. Data comes from the op catalog + live caps probe;
// this is the text the GUI shows when an op is opened (B12 indev). A NULL op yields a short placeholder.
void lxveos_gui_compose_detail(char *buf, size_t cap, const lxveos_op_t *op);
