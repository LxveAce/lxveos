// Host-side unit test for lxveos_gui_menu — the on-device GUI's pure text composition (no LVGL / esp_lcd).
// Covers the ARM/SAFE banner mapping, the per-op list-button label and the op detail card. These are driven
// off the real op catalog (lxveos_ops.c) + capability registry (lxveos_caps.c), compiled here against the host stubs
// (esp_log.h + a fixture sdkconfig.h that models a Wi-Fi board with a sub-GHz add-on). Built + run by
// tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_gui_menu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lxveos_caps.h"
#include "lxveos_ops.h"

static void test_arm_banner(void)
{
    char b[24];
    lxveos_gui_compose_arm_banner(b, sizeof(b), LXVEOS_ARM_SAFE);
    assert(strcmp(b, "SAFE") == 0);
    lxveos_gui_compose_arm_banner(b, sizeof(b), LXVEOS_ARM_PENDING);
    assert(strcmp(b, "ARM PENDING") == 0);
    lxveos_gui_compose_arm_banner(b, sizeof(b), LXVEOS_ARM_ARMED);
    assert(strcmp(b, "ARMED - TX LIVE") == 0);
    // An out-of-range state maps to the unambiguous placeholder, never leaks past the buffer.
    lxveos_gui_compose_arm_banner(b, sizeof(b), (lxveos_arm_state_t)99);
    assert(strcmp(b, "?") == 0);

    // cap==0 must be a safe no-op (doesn't touch buf); cap==1 yields just a terminator.
    char guard[4] = {'Z', 'Z', 'Z', 'Z'};
    lxveos_gui_compose_arm_banner(guard, 0, LXVEOS_ARM_ARMED);
    assert(guard[0] == 'Z');
    lxveos_gui_compose_arm_banner(guard, 1, LXVEOS_ARM_ARMED);
    assert(guard[0] == '\0');
}

static void test_arm_banner_color(void)
{
    // The colour half of the banner (0xRRGGBB), shared by the initial draw and the refresh timer so text +
    // colour never drift: red ARMED / amber PENDING / brand-green SAFE.
    assert(lxveos_gui_arm_banner_color(LXVEOS_ARM_ARMED) == 0xFF3030u);
    assert(lxveos_gui_arm_banner_color(LXVEOS_ARM_PENDING) == 0xFFB020u);
    assert(lxveos_gui_arm_banner_color(LXVEOS_ARM_SAFE) == 0x39FF14u);
    // An unknown state must never paint the "live" red — it falls back to the safe green.
    assert(lxveos_gui_arm_banner_color((lxveos_arm_state_t)99) == 0x39FF14u);
}

// Does `hay` contain `needle`? (thin strstr wrapper, keeps the asserts readable)
static int has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

// Find a catalog op by slug (NULL if absent).
static const lxveos_op_t *find_op(const char *slug)
{
    for (size_t i = 0; i < lxveos_ops_count(); i++) {
        const lxveos_op_t *op = lxveos_ops_get(i);
        if (op && strcmp(op->slug, slug) == 0) {
            return op;
        }
    }
    return NULL;
}

static void test_detail(void)
{
    lxveos_caps_probe();   // fixture: Wi-Fi active, sub-GHz add-on
    char d[512];

    // A ready recon op: title, slug, category/status, capability, source; no policy line (STD class).
    const lxveos_op_t *scan = find_op("wifi_ap_scan");
    assert(scan != NULL);
    lxveos_gui_compose_detail(d, sizeof(d), scan);
    assert(has(d, "Wi-Fi AP scan"));      // title
    assert(has(d, "slug: wifi_ap_scan"));
    assert(has(d, "recon"));              // category
    assert(has(d, "ready"));              // WIFI active + implemented -> ready
    assert(has(d, "needs: wifi"));        // required capability
    assert(has(d, "src: Marauder"));      // inspired_by
    assert(!has(d, "offensive") && !has(d, "restricted"));

    // An offensive op carries the ARM-required policy line.
    const lxveos_op_t *portal = find_op("evil_portal");
    assert(portal != NULL);
    lxveos_gui_compose_detail(d, sizeof(d), portal);
    assert(has(d, "Evil-portal captive portal"));
    assert(has(d, "offensive: requires ARM before TX"));

    // A restricted (DoS-class) op carries the upstream-TX policy line, not the arm line.
    const lxveos_op_t *burst = find_op("deauth_burst");
    assert(burst != NULL);
    lxveos_gui_compose_detail(d, sizeof(d), burst);
    assert(has(d, "restricted: owner/upstream-supplied TX"));
    assert(!has(d, "requires ARM"));

    // A sub-GHz add-on op reports "attachable" on this fixture board.
    const lxveos_op_t *sg = find_op("subghz_scan");
    assert(sg != NULL);
    lxveos_gui_compose_detail(d, sizeof(d), sg);
    assert(has(d, "attachable"));

    // NULL op -> safe placeholder; cap 0 -> no write; small cap -> truncated but terminated.
    lxveos_gui_compose_detail(d, sizeof(d), NULL);
    assert(strcmp(d, "(no op selected)") == 0);
    char z = 'Q';
    lxveos_gui_compose_detail(&z, 0, scan);
    assert(z == 'Q');
    char small[16];
    memset(small, 'G', sizeof(small));
    lxveos_gui_compose_detail(small, 12, scan);
    assert(strlen(small) < 12);
    for (size_t i = 12; i < sizeof(small); i++) {
        assert(small[i] == 'G');   // nothing written past cap
    }
}

static void test_op_label(void)
{
    // The per-op list-button label the interactive launcher (B12d) puts on each tappable row: glyph + slug +
    // policy tag, no leading space / newline. Fixture: Wi-Fi built in, sub-GHz add-on.
    lxveos_caps_probe();
    char lbl[64];

    // Ready STD recon op: glyph + slug, no tag.
    lxveos_gui_compose_op_label(lbl, sizeof(lbl), find_op("wifi_ap_scan"));
    assert(strcmp(lbl, "[+] wifi_ap_scan") == 0);
    // Ready offensive op: (arm) tag.
    lxveos_gui_compose_op_label(lbl, sizeof(lbl), find_op("evil_portal"));
    assert(strcmp(lbl, "[+] evil_portal (arm)") == 0);
    // Restricted DoS-class op: planned here (WIFI active, unimplemented) + (upstream) tag.
    lxveos_gui_compose_op_label(lbl, sizeof(lbl), find_op("deauth_burst"));
    assert(strcmp(lbl, "[.] deauth_burst (upstream)") == 0);
    // Add-on op, module not wired on this board: attachable glyph.
    lxveos_gui_compose_op_label(lbl, sizeof(lbl), find_op("subghz_scan"));
    assert(strcmp(lbl, "[~] subghz_scan") == 0);
    // Physically-impossible op: unavailable glyph (honesty — same as the menu renders).
    lxveos_gui_compose_op_label(lbl, sizeof(lbl), find_op("wifi_5ghz_scan"));
    assert(strcmp(lbl, "[x] wifi_5ghz_scan") == 0);

    // NULL op -> placeholder; cap 0 -> no write.
    lxveos_gui_compose_op_label(lbl, sizeof(lbl), NULL);
    assert(strcmp(lbl, "(none)") == 0);
    char z = 'Q';
    lxveos_gui_compose_op_label(&z, 0, find_op("wifi_ap_scan"));
    assert(z == 'Q');
}

int main(void)
{
    test_arm_banner();
    test_arm_banner_color();
    test_detail();
    test_op_label();
    printf("test_gui_menu: all tests passed\n");
    return 0;
}
