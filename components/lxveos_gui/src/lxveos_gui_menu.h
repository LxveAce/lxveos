#pragma once
// Internal text-composition helpers for the LxveOS on-device GUI, split out of lxveos_gui.c so the pure
// string building (no LVGL, no esp_lcd) can be host-unit-tested (tests/host_c/test_gui_menu.c). lxveos_gui.c
// wraps the results in LVGL labels.
#include <stddef.h>

#include "lxveos_arm.h"
#include "lxveos_ops.h"

// Compose the capability menu into `buf` (bounded by `cap`): a category header whenever the category changes,
// then one line per op — a status glyph ([+] ready / [.] planned / [~] attachable add-on / [x] unavailable),
// the op slug, and a policy tag ((arm) / (upstream)). Data comes straight from the op catalog.
void lxveos_gui_compose_menu(char *buf, size_t cap);

// Compose the ARM/SAFE banner text for the given arm state into `buf` (bounded by `cap`). Pure mapping of the
// two-factor arm state to a short, unambiguous banner the header shows (coloured by lxveos_gui.c).
void lxveos_gui_compose_arm_banner(char *buf, size_t cap, lxveos_arm_state_t state);

// Compose the multi-line detail card for one op into `buf` (bounded by `cap`): title, slug, category +
// runtime status, the capability it needs, its upstream inspiration, and — for offensive/restricted ops — a
// policy line spelling out the arm/upstream-TX requirement. Data comes from the op catalog + live caps probe;
// this is the text the GUI shows when an op is opened (B12 indev). A NULL op yields a short placeholder.
void lxveos_gui_compose_detail(char *buf, size_t cap, const lxveos_op_t *op);
