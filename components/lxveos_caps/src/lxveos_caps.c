// LxveOS capability registry (M0). Builds the compile-time capability mask from this board's
// CONFIG_LXVEOS_HAS_* symbols (generated from cyd_boards.json) and, at boot, seeds the runtime-active
// mask from it. Runtime-only add-on detection (CC1101 / nRF24 / PN532 on a bus, eFuse-gated silicon) is
// TODO(M1) and slots into lxveos_caps_probe() without touching callers — they gate on lxveos_cap_active().
#include "lxveos_caps.h"

#include <stddef.h>
#include <stdio.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "lxveos_caps";

static const char *const CAP_NAMES[LXVEOS_CAP_COUNT] = {
    [LXVEOS_CAP_WIFI]       = "wifi",
    [LXVEOS_CAP_BLE]        = "ble",
    [LXVEOS_CAP_BT_CLASSIC] = "bt_classic",
    [LXVEOS_CAP_DISPLAY]    = "display",
    [LXVEOS_CAP_STORAGE]    = "storage",
    [LXVEOS_CAP_GPS]        = "gps",
    [LXVEOS_CAP_IR]         = "ir",
    [LXVEOS_CAP_SUBGHZ]     = "subghz",
    [LXVEOS_CAP_NRF24]      = "nrf24",
    [LXVEOS_CAP_NFC]        = "nfc",
};

#define CAP_BIT(c) ((lxveos_cap_mask_t)1u << (c))

static lxveos_cap_mask_t s_active;  // resolved by lxveos_caps_probe(); 0 until then

// The set the image was compiled with: one bit per CONFIG_LXVEOS_HAS_* that is `y` for this board. A
// disabled bool Kconfig leaves its CONFIG_* undefined, so #ifdef is the correct test.
static lxveos_cap_mask_t builtin_mask(void)
{
    lxveos_cap_mask_t m = 0;
#ifdef CONFIG_LXVEOS_HAS_WIFI
    m |= CAP_BIT(LXVEOS_CAP_WIFI);
#endif
#ifdef CONFIG_LXVEOS_HAS_BLE
    m |= CAP_BIT(LXVEOS_CAP_BLE);
#endif
#ifdef CONFIG_LXVEOS_HAS_BT_CLASSIC
    m |= CAP_BIT(LXVEOS_CAP_BT_CLASSIC);
#endif
#ifdef CONFIG_LXVEOS_HAS_DISPLAY
    m |= CAP_BIT(LXVEOS_CAP_DISPLAY);
#endif
#ifdef CONFIG_LXVEOS_HAS_STORAGE
    m |= CAP_BIT(LXVEOS_CAP_STORAGE);
#endif
#ifdef CONFIG_LXVEOS_HAS_GPS
    m |= CAP_BIT(LXVEOS_CAP_GPS);
#endif
#ifdef CONFIG_LXVEOS_HAS_IR
    m |= CAP_BIT(LXVEOS_CAP_IR);
#endif
#ifdef CONFIG_LXVEOS_HAS_SUBGHZ
    m |= CAP_BIT(LXVEOS_CAP_SUBGHZ);
#endif
#ifdef CONFIG_LXVEOS_HAS_NRF24
    m |= CAP_BIT(LXVEOS_CAP_NRF24);
#endif
#ifdef CONFIG_LXVEOS_HAS_NFC
    m |= CAP_BIT(LXVEOS_CAP_NFC);
#endif
    return m;
}

lxveos_cap_mask_t lxveos_caps_builtin(void)
{
    return builtin_mask();
}

// The attachable add-ons for this board: one bit per CONFIG_LXVEOS_ADDON_* (a feature marked "addon" in
// cyd_boards.json — an external module on operator-supplied pins, not compiled/soldered in). Same #ifdef
// pattern as builtin_mask; a board can't list a cap as both HAS and ADDON (the manifest enforces one value).
static lxveos_cap_mask_t addon_mask(void)
{
    lxveos_cap_mask_t m = 0;
#ifdef CONFIG_LXVEOS_ADDON_STORAGE
    m |= CAP_BIT(LXVEOS_CAP_STORAGE);
#endif
#ifdef CONFIG_LXVEOS_ADDON_GPS
    m |= CAP_BIT(LXVEOS_CAP_GPS);
#endif
#ifdef CONFIG_LXVEOS_ADDON_IR
    m |= CAP_BIT(LXVEOS_CAP_IR);
#endif
#ifdef CONFIG_LXVEOS_ADDON_SUBGHZ
    m |= CAP_BIT(LXVEOS_CAP_SUBGHZ);
#endif
#ifdef CONFIG_LXVEOS_ADDON_NRF24
    m |= CAP_BIT(LXVEOS_CAP_NRF24);
#endif
#ifdef CONFIG_LXVEOS_ADDON_NFC
    m |= CAP_BIT(LXVEOS_CAP_NFC);
#endif
    return m;
}

lxveos_cap_mask_t lxveos_caps_addon(void)
{
    return addon_mask();
}

bool lxveos_cap_is_addon(lxveos_cap_t cap)
{
    if ((int)cap < 0 || cap >= LXVEOS_CAP_COUNT) {
        return false;
    }
    return (addon_mask() & CAP_BIT(cap)) != 0;
}

const char *lxveos_cap_name(lxveos_cap_t cap)
{
    if ((int)cap < 0 || cap >= LXVEOS_CAP_COUNT || CAP_NAMES[cap] == NULL) {
        return "?";
    }
    return CAP_NAMES[cap];
}

bool lxveos_cap_active(lxveos_cap_t cap)
{
    if ((int)cap < 0 || cap >= LXVEOS_CAP_COUNT) {
        return false;
    }
    return (s_active & CAP_BIT(cap)) != 0;
}

lxveos_cap_mask_t lxveos_caps_active(void)
{
    return s_active;
}

lxveos_cap_mask_t lxveos_caps_probe(void)
{
    // M0: the active set == the compile-time set. TODO(M1): OR in add-on buses (CC1101/nRF24/PN532) and
    // eFuse-gated silicon features detected here; nothing above lxveos_cap_active() needs to change.
    s_active = builtin_mask();

    char line[160];
    size_t n = 0;
    line[0] = '\0';
    for (int c = 0; c < LXVEOS_CAP_COUNT; c++) {
        if (!(s_active & CAP_BIT(c))) {
            continue;
        }
        int w = snprintf(line + n, sizeof(line) - n, "%s%s", n ? " " : "", lxveos_cap_name((lxveos_cap_t)c));
        if (w > 0 && (size_t)w < sizeof(line) - n) {
            n += (size_t)w;
        }
    }
    ESP_LOGI(TAG, "capabilities (%d active): %s", __builtin_popcount((unsigned)s_active), n ? line : "(none)");
    return s_active;
}
