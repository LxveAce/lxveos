// LxveOS app entry (M0 scaffold). Thin: banner -> board bring-up -> control surface. The capability
// probe (eFuse/bus) -> feature registry and the per-ui_profile UI loop are TODO(M0/M1).
#include "esp_log.h"
#include "lxveos_board.h"
#include "lxveos_cli.h"

#define LXVEOS_VERSION "0.1.0-m0"

static const char *TAG = "lxveos";

void app_main(void)
{
    ESP_LOGI(TAG, "LxveOS %s — by LxveLabs, built by LxveAce", LXVEOS_VERSION);
    ESP_ERROR_CHECK(lxveos_board_init());
    ESP_ERROR_CHECK(lxveos_cli_start());
    // TODO(M0): lxveos_capabilities probe -> lxveos_registry (compile-time HAS_* + runtime CAP_* gating).
    // TODO(M0): if ui_profile != headless, start esp_lvgl_port UI; else stay in the serial loop.
}
