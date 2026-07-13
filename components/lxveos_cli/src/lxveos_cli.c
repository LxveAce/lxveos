// LxveOS serial CLI (M0). An esp_console REPL is the headless control surface on every board — the
// interactive console now, and the versioned Cyber Controller bridge protocol later, both live here.
// Commands read the capability registry (lxveos_caps) so they report exactly what this unit can do.
// The console transport is chip-agnostic: UART where that's the default console, USB-Serial-JTAG or
// USB-CDC otherwise — the CONFIG_ESP_CONSOLE_* guards below pick the one this image was built with, so
// the same code compiles on classic ESP32 (UART) and S3 (often USB-Serial-JTAG).
//
// Everything except `agree`/`help` is locked until the operator accepts the authorized-use terms once.
// The acceptance is persisted in NVS (ns "lxveos", key "use_ack"), so it is a genuine first-run gate,
// not a per-boot nag — a deliberate ethics boundary for a security-research firmware.
#include "lxveos_cli.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "bsp/display.h"
#include "lxveos_board.h"
#include "lxveos_caps.h"

#define LXVEOS_NVS_NS      "lxveos"
#define LXVEOS_NVS_USE_ACK "use_ack"
#define LXVEOS_NVS_BOOTCNT "boot_count"
// User keys from the `nvs` command are prefixed so they can never collide with the internal keys above
// (NVS keys are capped at 15 chars, so a user key is limited to 13).
#define LXVEOS_NVS_USERPFX "u_"

static const char *TAG = "lxveos_cli";

// True once the operator has accepted the authorized-use terms (mirrors the NVS-persisted flag).
static bool s_use_ack;

// This unit's lifetime boot count, read+incremented once per boot (0 if NVS is unavailable).
static uint32_t s_boot_count;

static bool read_use_ack(void)
{
    nvs_handle_t h;
    if (nvs_open(LXVEOS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;  // namespace not created yet -> never acked
    }
    uint8_t v = 0;
    esp_err_t r = nvs_get_u8(h, LXVEOS_NVS_USE_ACK, &v);
    nvs_close(h);
    return r == ESP_OK && v == 1;
}

static void persist_use_ack(void)
{
    nvs_handle_t h;
    if (nvs_open(LXVEOS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not persist authorized-use ack (NVS open failed)");
        return;
    }
    if (nvs_set_u8(h, LXVEOS_NVS_USE_ACK, 1) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// Read this unit's lifetime boot counter, increment it, and persist. Called once per boot; leaves
// s_boot_count at 0 if NVS is unavailable (a missing key just starts the count at 1).
static void bump_boot_count(void)
{
    nvs_handle_t h;
    if (nvs_open(LXVEOS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t n = 0;
    nvs_get_u32(h, LXVEOS_NVS_BOOTCNT, &n);  // ESP_ERR_NVS_NOT_FOUND leaves n == 0
    n++;
    if (nvs_set_u32(h, LXVEOS_NVS_BOOTCNT, n) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    s_boot_count = n;
}

// Commands other than `agree` refuse to run until the terms are accepted. Returns 0 either way so the
// REPL doesn't print a scary "non-zero error code" after the notice.
static bool locked(void)
{
    if (s_use_ack) {
        return false;
    }
    printf("locked — type 'agree' to accept the authorized-use terms (see RESPONSIBLE-USE.md) first.\n");
    return true;
}

// `agree` — accept the authorized-use terms; persisted so later boots start unlocked.
static int cmd_agree(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_use_ack) {
        s_use_ack = true;
        persist_use_ack();
    }
    printf("Acknowledged. Authorized, lawful security research & education only. Commands unlocked.\n");
    return 0;
}

// `info` — identity of this unit: firmware version, board id, the chip it was built for, and UI profile.
static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    printf("fw    : LxveOS %s\n", LXVEOS_VERSION);
    printf("board : %s\n", lxveos_board_id());
    printf("chip  : %s\n", lxveos_board_chip());
    printf("ui    : %s\n", lxveos_ui_profile());
    return 0;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "other-wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

// `sysinfo` — runtime state: ESP-IDF version, why we last reset, and heap headroom (current + worst-case
// since boot). The counterpart to `info` (static identity).
static int cmd_sysinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    printf("idf     : %s\n", esp_get_idf_version());
    printf("reset   : %s\n", reset_reason_str(esp_reset_reason()));
    printf("boot #  : %u\n", (unsigned)s_boot_count);
    printf("uptime  : %llu s\n", (unsigned long long)(esp_timer_get_time() / 1000000));
    printf("heap    : %u free / %u min-free (bytes)\n",
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    return 0;
}

// `status` — ONE machine-readable line the Cyber Controller host parses to identify this unit. Versioned
// prefix "LXVEOS/1 status" + space-separated key=value fields (values are safe slugs / hex / decimal, no
// embedded spaces). This is the seed of the M1 CC bridge protocol; M1 will emit an equivalent identity
// line at boot, framed and outside this ack gate, for headless host auto-detection.
static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    const char *panel = bsp_display_panel();
    printf("LXVEOS/1 status board=%s chip=%s ui=%s fw=%s panel=%s caps=0x%03x heap=%u\n",
           lxveos_board_id(), lxveos_board_chip(), lxveos_ui_profile(), LXVEOS_VERSION,
           (panel && panel[0]) ? panel : "none",
           (unsigned)lxveos_caps_active(), (unsigned)esp_get_free_heap_size());
    return 0;
}

// `loglevel <tag|*> <level>` — set the runtime ESP-IDF log verbosity for one tag (or `*` = all), so an
// operator can quiet a noisy subsystem or turn on debug without a reflash. Level names map to esp_log_level_t.
static int cmd_loglevel(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (argc != 3) {
        printf("usage: loglevel <tag|*> <none|error|warn|info|debug|verbose>\n");
        return 0;
    }
    const char *tag = argv[1];
    const char *lvl = argv[2];
    esp_log_level_t level;
    if (strcmp(lvl, "none") == 0) {
        level = ESP_LOG_NONE;
    } else if (strcmp(lvl, "error") == 0) {
        level = ESP_LOG_ERROR;
    } else if (strcmp(lvl, "warn") == 0) {
        level = ESP_LOG_WARN;
    } else if (strcmp(lvl, "info") == 0) {
        level = ESP_LOG_INFO;
    } else if (strcmp(lvl, "debug") == 0) {
        level = ESP_LOG_DEBUG;
    } else if (strcmp(lvl, "verbose") == 0) {
        level = ESP_LOG_VERBOSE;
    } else {
        printf("unknown level '%s' (want none/error/warn/info/debug/verbose)\n", lvl);
        return 0;
    }
    esp_log_level_set(tag, level);
    printf("log level for '%s' set to %s\n", tag, lvl);
    return 0;
}

// `reboot` — restart the unit. esp_restart() does not return, so control never falls off the end.
static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    printf("rebooting...\n");
    fflush(stdout);
    esp_restart();
}

// `nvs get <key>` / `nvs set <key> <value>` — a small persistent string store for operator settings, kept
// in the "lxveos" namespace but under a `u_` prefix so it can't touch the firmware's own keys.
static int cmd_nvs(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    bool is_get = argc == 3 && strcmp(argv[1], "get") == 0;
    bool is_set = argc == 4 && strcmp(argv[1], "set") == 0;
    if (!is_get && !is_set) {
        printf("usage: nvs get <key> | nvs set <key> <value>\n");
        return 0;
    }
    char nk[16];
    int w = snprintf(nk, sizeof(nk), LXVEOS_NVS_USERPFX "%s", argv[2]);
    if (w < 0 || (size_t)w >= sizeof(nk)) {
        printf("key too long (max 13 chars)\n");
        return 0;
    }
    nvs_handle_t h;
    if (nvs_open(LXVEOS_NVS_NS, is_set ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) {
        printf("nvs unavailable\n");
        return 0;
    }
    if (is_get) {
        char val[128];
        size_t len = sizeof(val);
        esp_err_t r = nvs_get_str(h, nk, val, &len);
        if (r == ESP_OK) {
            printf("%s = %s\n", argv[2], val);
        } else if (r == ESP_ERR_NVS_NOT_FOUND) {
            printf("%s: not set\n", argv[2]);
        } else {
            printf("%s: read error\n", argv[2]);
        }
    } else {
        if (nvs_set_str(h, nk, argv[3]) == ESP_OK && nvs_commit(h) == ESP_OK) {
            printf("%s = %s (saved)\n", argv[2], argv[3]);
        } else {
            printf("%s: write failed\n", argv[2]);
        }
    }
    nvs_close(h);
    return 0;
}

// `caps` — the capability registry: every capability and whether the boot probe left it active.
static int cmd_caps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    for (int c = 0; c < LXVEOS_CAP_COUNT; c++) {
        printf("  %-11s %s\n", lxveos_cap_name((lxveos_cap_t)c),
               lxveos_cap_active((lxveos_cap_t)c) ? "active" : "-");
    }
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "agree", .help = "Accept the authorized-use terms to unlock commands", .func = &cmd_agree},
        {.command = "info", .help = "Show firmware version, board id, chip and UI profile", .func = &cmd_info},
        {.command = "caps", .help = "List capabilities and whether each is active", .func = &cmd_caps},
        {.command = "sysinfo", .help = "Show ESP-IDF version, reset reason and heap free", .func = &cmd_sysinfo},
        {.command = "status", .help = "One machine-readable status line (Cyber Controller bridge format)", .func = &cmd_status},
        {.command = "loglevel", .help = "Set log verbosity: loglevel <tag|*> <none|error|warn|info|debug|verbose>", .func = &cmd_loglevel},
        {.command = "reboot", .help = "Restart the unit", .func = &cmd_reboot},
        {.command = "nvs", .help = "Persistent settings: nvs get <key> | nvs set <key> <value>", .func = &cmd_nvs},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

esp_err_t lxveos_cli_start(void)
{
    s_use_ack = read_use_ack();
    bump_boot_count();

    ESP_LOGI(TAG, "LxveOS ready on '%s' (ui: %s, boot #%u).", lxveos_board_id(), lxveos_ui_profile(),
             (unsigned)s_boot_count);
    ESP_LOGW(TAG, "Authorized, lawful security research & education only. No jammer. See RESPONSIBLE-USE.md.");
    if (!s_use_ack) {
        ESP_LOGW(TAG, "First run: commands are LOCKED until you type 'agree' to accept the authorized-use terms.");
    }

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

    // TODO(M1): versioned single-line UART protocol (PCAP + text) = the Cyber Controller bridge SSOT.
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return ESP_OK;
}
