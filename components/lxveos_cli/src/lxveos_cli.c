// LxveOS serial CLI (M0 scaffold). Prints the banner + authorized-use gate; the interactive console
// (esp_console) and the versioned Cyber Controller bridge protocol are TODO(M0/M1).
#include "lxveos_cli.h"
#include "lxveos_board.h"
#include "esp_log.h"

static const char *TAG = "lxveos_cli";

esp_err_t lxveos_cli_start(void)
{
    ESP_LOGI(TAG, "LxveOS ready on '%s' (ui: %s). Serial control surface: TODO(M0 esp_console).",
             lxveos_board_id(), lxveos_ui_profile());
    ESP_LOGW(TAG, "Authorized, lawful security research & education only. No jammer. See RESPONSIBLE-USE.md.");
    // TODO(M0): esp_console REPL (help/scan/monitor/...) gated behind first-run authorized-use ack.
    // TODO(M1): versioned UART protocol (single-line PCAP+text) = the Cyber Controller bridge SSOT.
    return ESP_OK;
}
