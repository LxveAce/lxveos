// LxveOS serial CLI (M0). An esp_console REPL is the headless control surface on every board — the
// interactive console now, and the versioned Cyber Controller bridge protocol later, both live here.
// Commands read the capability registry (lxveos_caps) so they report exactly what this unit can do.
// The console transport is chip-agnostic: UART where that's the default console, USB-Serial-JTAG or
// USB-CDC otherwise — the CONFIG_ESP_CONSOLE_* guards below pick the one this image was built with, so
// the same code compiles on classic ESP32 (UART) and S3 (often USB-Serial-JTAG).
#include "lxveos_cli.h"

#include <stdio.h>

#include "esp_console.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "lxveos_board.h"
#include "lxveos_caps.h"

static const char *TAG = "lxveos_cli";

// `info` — identity of this unit: board id, the chip it was built for, and the UI profile.
static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("board : %s\n", lxveos_board_id());
    printf("chip  : %s\n", lxveos_board_chip());
    printf("ui    : %s\n", lxveos_ui_profile());
    return 0;
}

// `caps` — the capability registry: every capability and whether the boot probe left it active.
static int cmd_caps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int c = 0; c < LXVEOS_CAP_COUNT; c++) {
        printf("  %-11s %s\n", lxveos_cap_name((lxveos_cap_t)c),
               lxveos_cap_active((lxveos_cap_t)c) ? "active" : "-");
    }
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "info", .help = "Show board id, chip and UI profile", .func = &cmd_info},
        {.command = "caps", .help = "List capabilities and whether each is active", .func = &cmd_caps},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

esp_err_t lxveos_cli_start(void)
{
    ESP_LOGI(TAG, "LxveOS ready on '%s' (ui: %s).", lxveos_board_id(), lxveos_ui_profile());
    ESP_LOGW(TAG, "Authorized, lawful security research & education only. No jammer. See RESPONSIBLE-USE.md.");

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lxveos>";
    repl_config.max_cmdline_length = 256;

    // Bind the REPL to whichever console transport this image selected (esp_console_new_repl_uart also
    // registers the built-in 'help' command). The guards keep the config macros — which reference
    // transport-specific CONFIG_* symbols — from being expanded on a target that doesn't use them.
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
#error "LxveOS: no supported console (enable a UART / USB-Serial-JTAG / USB-CDC console in menuconfig)"
#endif

    register_commands();

    // TODO(M0): gate the REPL behind a first-run authorized-use ack persisted to NVS.
    // TODO(M1): versioned single-line UART protocol (PCAP + text) = the Cyber Controller bridge SSOT.
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return ESP_OK;
}
