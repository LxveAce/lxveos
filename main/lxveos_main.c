// LxveOS app entry (M0 scaffold). Thin: banner -> board bring-up -> capability probe -> control surface.
// The capability registry (lxveos_caps) resolves the active feature set the CLI/UI/CC-bridge gate on; the
// per-ui_profile LVGL UI loop is TODO(M1).
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lxveos_board.h"
#include "lxveos_caps.h"
#include "lxveos_cli.h"

#define LXVEOS_VERSION "0.1.0-m0"

static const char *TAG = "lxveos";

// NVS must be up before board bring-up: the CYD panel-identity cache and (later) the CLI authorized-use
// ack both persist there. Standard init with an erase-and-retry when the partition is stale.
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "LxveOS %s — by LxveLabs, built by LxveAce", LXVEOS_VERSION);
    init_nvs();
    ESP_ERROR_CHECK(lxveos_board_init());  // resolves + caches the CYD panel identity via NVS
    lxveos_caps_probe();  // compile-time HAS_* -> runtime-active CAP_* set (M0); logs what's live
    ESP_ERROR_CHECK(lxveos_cli_start());
    // TODO(M1): if ui_profile != headless, start esp_lvgl_port UI; else stay in the serial loop.
    // TODO(M1): lxveos_caps_probe() gains bus/eFuse add-on detection (CC1101/nRF24/PN532).
}
