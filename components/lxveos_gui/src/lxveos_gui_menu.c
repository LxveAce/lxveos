// lxveos_gui_menu — pure text composition for the on-device GUI (see lxveos_gui_menu.h). No LVGL / esp_lcd,
// so the menu + arm-banner string building is host-unit-tested off-target. Extracted verbatim from
// lxveos_gui.c (compose_menu) plus the new arm banner; behaviour-preserving.
#include "lxveos_gui_menu.h"

#include <stddef.h>   // NULL
#include <stdio.h>

#include "lxveos_ops.h"

void lxveos_gui_compose_menu(char *buf, size_t cap)
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

void lxveos_gui_compose_arm_banner(char *buf, size_t cap, lxveos_arm_state_t state)
{
    if (cap == 0) {
        return;
    }
    const char *s;
    switch (state) {
    case LXVEOS_ARM_ARMED:   s = "ARMED - TX LIVE"; break;
    case LXVEOS_ARM_PENDING: s = "ARM PENDING";     break;
    case LXVEOS_ARM_SAFE:    s = "SAFE";            break;
    default:                 s = "?";               break;
    }
    snprintf(buf, cap, "%s", s);
}
