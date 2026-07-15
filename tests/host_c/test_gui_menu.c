// Host-side unit test for lxveos_gui_menu — the on-device GUI's pure text composition (no LVGL / esp_lcd).
// Covers the ARM/SAFE banner mapping and the capability-menu builder. The menu is driven off the real op
// catalog (lxveos_ops.c) + capability registry (lxveos_caps.c), compiled here against the host stubs
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

// Does `hay` contain `needle`? (thin strstr wrapper, keeps the asserts readable)
static int has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static void test_menu_content(void)
{
    // Fixture board (stubs/sdkconfig.h): Wi-Fi built in, sub-GHz attachable, everything else absent.
    lxveos_caps_probe();
    assert(lxveos_cap_active(LXVEOS_CAP_WIFI));
    assert(!lxveos_cap_active(LXVEOS_CAP_SUBGHZ));
    assert(lxveos_cap_is_addon(LXVEOS_CAP_SUBGHZ));

    char menu[4000];
    lxveos_gui_compose_menu(menu, sizeof(menu));

    // Every category header the catalog uses shows up.
    assert(has(menu, "recon"));
    assert(has(menu, "attack"));
    assert(has(menu, "defense"));
    assert(has(menu, "logging"));
    assert(has(menu, "misc"));

    // Known slugs land in the menu.
    assert(has(menu, "wifi_ap_scan"));
    assert(has(menu, "subghz_scan"));

    // All four status glyphs appear on this fixture: Wi-Fi ops ready [+] and planned [.], sub-GHz add-on
    // ops attachable [~], and the caps this board can't host (BLE/NFC/IR/...) unavailable [x].
    assert(has(menu, "[+]"));   // wifi_ap_scan is implemented + WIFI active
    assert(has(menu, "[.]"));   // wifi_5ghz_scan planned (WIFI active, not implemented)
    assert(has(menu, "[~]"));   // subghz_* attachable add-on
    assert(has(menu, "[x]"));   // ble_scan etc. unavailable

    // A ready Wi-Fi op sits on the same line as the [+] glyph and slug.
    assert(has(menu, "[+] wifi_ap_scan"));
    // Policy tags render: offensive ops carry (arm), the DoS class carries (upstream).
    assert(has(menu, "(arm)"));        // e.g. evil_portal / karma_ap
    assert(has(menu, "(upstream)"));   // e.g. deauth_burst / beacon_flood
    // ...and a plain recon op carries no policy tag (no stray tag bleed).
    assert(has(menu, "wifi_ap_scan\n"));
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

static void test_menu_bounds(void)
{
    // Small buffer: the builder must truncate cleanly — always NUL-terminated, never writing past `cap`.
    // Wrap the target in a sentinel-guarded array and confirm the guard byte is untouched.
    lxveos_caps_probe();
    char arena[80];
    memset(arena, 'G', sizeof(arena));
    const size_t cap = 40;
    lxveos_gui_compose_menu(arena, cap);
    assert(strlen(arena) < cap);          // fits, terminated inside cap
    for (size_t i = cap; i < sizeof(arena); i++) {
        assert(arena[i] == 'G');          // nothing written past cap
    }

    // cap==0 is a documented no-op (must not dereference buf[0]).
    char z = 'Q';
    lxveos_gui_compose_menu(&z, 0);
    assert(z == 'Q');

    // cap==1 gives an empty, terminated string.
    char one[1] = {'X'};
    lxveos_gui_compose_menu(one, sizeof(one));
    assert(one[0] == '\0');
}

int main(void)
{
    test_arm_banner();
    test_menu_content();
    test_detail();
    test_menu_bounds();
    printf("test_gui_menu: all tests passed\n");
    return 0;
}
