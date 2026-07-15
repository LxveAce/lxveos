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
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "bsp/display.h"
#include "lxveos_arm.h"
#include "lxveos_ble.h"
#include "lxveos_ir.h"
#include "lxveos_subghz.h"
#include "lxveos_radiomath.h"
#include "lxveos_nrf24.h"
#include "lxveos_nfc.h"
#include "lxveos_board.h"
#include "lxveos_caps.h"
#include "lxveos_cliutil.h"
#include "lxveos_evilportal.h"
#include "lxveos_evt.h"
#include "lxveos_ops.h"
#include "lxveos_wifi.h"

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
//
// `ops=<ready>/<planned>/<unavailable>` summarises the operation catalog (see `features`): how many
// security operations this unit can run now, has planned for a capability it HAS, and can't do for lack
// of a capability — so the host sees each unit's feature surface without issuing a second command. Unknown
// keys are safe to append: the host parser keys on field names, so older hosts ignore `ops` transparently.
static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    size_t ops_ready = 0, ops_planned = 0, ops_attach = 0, ops_unavail = 0;
    lxveos_ops_tally(&ops_ready, &ops_planned, &ops_attach, &ops_unavail);
    const char *panel = bsp_display_panel();
    // tx= reports whether offensive-TX is COMPILED IN (a LXVEOS_TX_DISABLE build reports tx=0), so the CC
    // host can tell a TX-capable-but-SAFE unit from one that can never arm — both otherwise show arm=safe.
    printf("LXVEOS/1 status board=%s chip=%s ui=%s fw=%s panel=%s caps=0x%03x ops=%u/%u/%u heap=%u arm=%s tx=%d\n",
           lxveos_board_id(), lxveos_board_chip(), lxveos_ui_profile(), LXVEOS_VERSION,
           (panel && panel[0]) ? panel : "none",
           (unsigned)lxveos_caps_active(),
           // Machine contract: ops=ready/planned/not-ready. Attachable add-on ops fold into the 3rd field so
           // the CC bridge sees the same 3-number format (and the same total) as before the attachable split.
           (unsigned)ops_ready, (unsigned)ops_planned, (unsigned)(ops_attach + ops_unavail),
           (unsigned)esp_get_free_heap_size(), lxveos_arm_state_name(lxveos_arm_state()),
           lxveos_arm_tx_compiled() ? 1 : 0);
    return 0;
}

// `bridge on|off|status` — toggle machine-readable LXVEOS/1 event emission (see docs/EVENT-PROTOCOL.md). Off by
// default so the interactive console stays clean; the Cyber Controller sends `bridge on` after connect so the
// recon/defense/capture/arm ops stream typed `LXVEOS/1 <type> k=v` lines it can parse. The `status` line is
// always available regardless of this toggle.
static int cmd_bridge(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        lxveos_evt_set_enabled(true);
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        lxveos_evt_set_enabled(false);
    }
    printf("LXVEOS/1 bridge state=%s\n", lxveos_evt_enabled() ? "on" : "off");
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

// `features` — the operation catalog: every security operation LxveOS plans to offer (drawn from
// Marauder and the wider firmware landscape), each with its live status on THIS unit. Status is derived
// from the capability registry: "planned" = the required radio/peripheral is present and the driver
// lands in M1+, "unavailable" = this board lacks the capability, "ready" = actually implemented. Attack
// operations are shown + labelled (never hidden), but LxveOS authors no jammer/deauth transmit frames.
static int cmd_features(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    printf("LxveOS operation catalog (CI-green, not yet hardware-validated; attack ops are arm-gated, lab-only)\n");
    for (size_t i = 0; i < lxveos_ops_count(); i++) {
        const lxveos_op_t *op = lxveos_ops_get(i);
        if (op == NULL) {
            continue;
        }
        printf("  [%-11s] %-8s %-10s %-14s %-22s (%s, ~%s)\n",
               lxveos_op_status_name(lxveos_op_status(op)),
               lxveos_opcat_name(op->category), lxveos_op_class_name(lxveos_op_class(op)),
               op->slug, op->title,
               lxveos_cap_name(op->required_cap), op->inspired_by);
    }
    size_t ready = 0, planned = 0, attachable = 0, unavailable = 0;
    lxveos_ops_tally(&ready, &planned, &attachable, &unavailable);
    printf("summary: %u ready / %u planned / %u attachable / %u unavailable  "
           "(attachable = wire the add-on module; LxveOS authors no jammer/DoS-flood frames)\n",
           (unsigned)ready, (unsigned)planned, (unsigned)attachable, (unsigned)unavailable);
    return 0;
}

// `scan` — passive Wi-Fi AP scan (the `wifi_ap_scan` catalog operation). Listens for beacons only and
// transmits nothing; gated on the WIFI capability. Prints a table of the access points in range.
static int cmd_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — nothing to scan\n");
        return 0;
    }
    printf("passive Wi-Fi AP scan (listening only — no frames transmitted)...\n");
    static lxveos_wifi_ap_t aps[32];
    size_t found = 0;
    esp_err_t e = lxveos_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);
    if (e != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("  %-32s %5s %3s %-8s %s\n", "SSID", "RSSI", "CH", "AUTH", "BSSID");
    for (size_t i = 0; i < found; i++) {
        printf("  %-32s %4ddB %3u %-8s %02x:%02x:%02x:%02x:%02x:%02x\n",
               aps[i].ssid[0] ? aps[i].ssid : "<hidden>",
               aps[i].rssi, aps[i].channel, lxveos_wifi_authmode_str(aps[i].authmode),
               aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
               aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
    }
    if (lxveos_evt_enabled()) {
        for (size_t i = 0; i < found; i++) {
            char line[192];
            size_t n = lxveos_evt_begin(line, sizeof(line), "ap");
            n = lxveos_evt_kv_mac(line, sizeof(line), n, "bssid", aps[i].bssid);
            n = lxveos_evt_kv_hex(line, sizeof(line), n, "ssid",
                                  (const uint8_t *)aps[i].ssid, strlen(aps[i].ssid));
            n = lxveos_evt_kv_int(line, sizeof(line), n, "ch", aps[i].channel);
            n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", aps[i].rssi);
            n = lxveos_evt_kv(line, sizeof(line), n, "auth", lxveos_wifi_authmode_str(aps[i].authmode));
            printf("%s\n", line);
        }
        char done[64];
        size_t dn = lxveos_evt_begin(done, sizeof(done), "done");
        dn = lxveos_evt_kv(done, sizeof(done), dn, "of", "scan");
        dn = lxveos_evt_kv_uint(done, sizeof(done), dn, "n", (unsigned long)found);
        printf("%s\n", done);
    }
    printf("%u AP(s) in range\n", (unsigned)found);
    return 0;
}

// `sniff [seconds]` — passive Wi-Fi packet monitor (the `wifi_sniff` catalog operation). Enables
// promiscuous RX and channel-hops the 2.4 GHz plan, tallying frames by 802.11 type and channel. Listens
// only — transmits nothing, and captures no payloads/PII (counts only). WIFI-capability gated.
static int cmd_sniff(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot sniff\n");
        return 0;
    }
    uint32_t secs = 8;
    uint8_t channel = 0;  // 0 = hop all channels
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: sniff [seconds 1-60] [channel 1-13]  (default 8s, all channels)\n");
            return 0;
        }
    }
    if (argc >= 3) {
        long c = strtol(argv[2], NULL, 10);
        if (c >= 1 && c <= 13) {
            channel = (uint8_t)c;
        } else {
            printf("usage: sniff [seconds 1-60] [channel 1-13]  (default 8s, all channels)\n");
            return 0;
        }
    }
    if (channel) {
        printf("passive Wi-Fi packet monitor for %us on channel %u (promiscuous listen — no frames sent)...\n",
               (unsigned)secs, channel);
    } else {
        printf("passive Wi-Fi packet monitor for %us, all channels (promiscuous listen — no frames sent)...\n",
               (unsigned)secs);
    }
    lxveos_wifi_sniff_stats_t st;
    esp_err_t e = lxveos_wifi_sniff(secs, channel, &st);
    if (e != ESP_OK) {
        printf("sniff failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("frames: %u total — mgmt %u, data %u, ctrl %u, misc %u  (%u channel dwells)\n",
           (unsigned)st.total, (unsigned)st.mgmt, (unsigned)st.data, (unsigned)st.ctrl,
           (unsigned)st.misc, (unsigned)st.channels_swept);
    printf("per channel:");
    bool any = false;
    for (int c = 1; c <= 13; c++) {
        if (st.per_channel[c]) {
            printf(" ch%d=%u", c, (unsigned)st.per_channel[c]);
            any = true;
        }
    }
    printf("%s\n", any ? "" : " (none)");
    return 0;
}

// Sink for the hashcat-22000 lines the capture produces — printed one per line, indented.
static void capture_emit_line(const char *line)
{
    printf("  %s\n", line);
    // Bridge: forward the ready-to-crack artifact straight to CC's Crack Lab. A hashcat-22000 line is
    // already token-safe (no spaces — its ESSID field is hex) but can run several hundred bytes for a
    // WPA*02 handshake, so emit it with a direct printf rather than the bounded evt builder (no fixed
    // buffer to truncate it). `kind` distinguishes the WPA*01 PMKID from the WPA*02 EAPOL handshake.
    if (lxveos_evt_enabled() && line != NULL) {
        const char *kind = (strncmp(line, "WPA*01*", 7) == 0) ? "pmkid" : "eapol";
        printf("LXVEOS/1 hs kind=%s line=%s\n", kind, line);
    }
}

// `capture [seconds]` — passive EAPOL/PMKID capture (the `eapol_capture` catalog operation). Listens in
// promiscuous mode, parses beacons for ESSIDs and EAPOL-Key frames for the 4-way handshake, and prints a
// hashcat-22000 WPA*01 line for any RSN PMKID seen (feeds the Cyber Controller WPA crack pipeline). LISTEN
// ONLY — never transmits a deauth to force a handshake; captures only what is already in the air.
static int cmd_capture(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot capture\n");
        return 0;
    }
    uint32_t secs = 15;
    uint8_t channel = 0;  // 0 = hop all channels
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 120) {
            secs = (uint32_t)v;
        } else {
            printf("usage: capture [seconds 1-120] [channel 1-13]  (default 15s, all channels)\n");
            return 0;
        }
    }
    if (argc >= 3) {
        long c = strtol(argv[2], NULL, 10);
        if (c >= 1 && c <= 13) {
            channel = (uint8_t)c;
        } else {
            printf("usage: capture [seconds 1-120] [channel 1-13]  (default 15s, all channels)\n");
            return 0;
        }
    }
    if (channel) {
        printf("passive EAPOL/PMKID capture for %us on channel %u (listen only — never forces a handshake)...\n",
               (unsigned)secs, channel);
    } else {
        printf("passive EAPOL/PMKID capture for %us, all channels (listen only — never forces a handshake)...\n",
               (unsigned)secs);
    }
    lxveos_wifi_eapol_stats_t st;
    esp_err_t e = lxveos_wifi_eapol_capture(secs, channel, capture_emit_line, &st);
    if (e != ESP_OK) {
        printf("capture failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("beacons %u (%u ESSIDs) — EAPOL %u (M1 %u M2 %u M3 %u M4 %u) — PMKIDs %u — handshakes %u — %u channel dwells\n",
           (unsigned)st.beacons, (unsigned)st.essids, (unsigned)st.eapol_frames,
           (unsigned)st.m1, (unsigned)st.m2, (unsigned)st.m3, (unsigned)st.m4,
           (unsigned)st.pmkids, (unsigned)st.mics, (unsigned)st.channels_swept);
    if (st.eapol_frames == 0) {
        printf("(no EAPOL in the air this window — no client (re)associated; honest result)\n");
    } else {
        printf("(emitted %u WPA*01 PMKID + %u WPA*02 handshake hashcat-22000 line(s))\n",
               (unsigned)st.pmkids, (unsigned)st.mics);
    }
    return 0;
}

// `stations [seconds]` — passive client-station scan (the `wifi_sta_scan` catalog operation). Infers
// client<->AP links from data-frame addresses and lists each client with its AP's ESSID, frame count and
// signal. Listens only — transmits nothing.
static int cmd_stations(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot scan for stations\n");
        return 0;
    }
    uint32_t secs = 12;
    uint8_t channel = 0;  // 0 = hop all channels
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 120) {
            secs = (uint32_t)v;
        } else {
            printf("usage: stations [seconds 1-120] [channel 1-13]  (default 12s, all channels)\n");
            return 0;
        }
    }
    if (argc >= 3) {
        long c = strtol(argv[2], NULL, 10);
        if (c >= 1 && c <= 13) {
            channel = (uint8_t)c;
        } else {
            printf("usage: stations [seconds 1-120] [channel 1-13]  (default 12s, all channels)\n");
            return 0;
        }
    }
    if (channel) {
        printf("passive Wi-Fi station scan for %us on channel %u (inferring clients from data frames — no frames sent)...\n",
               (unsigned)secs, channel);
    } else {
        printf("passive Wi-Fi station scan for %us, all channels (inferring clients from data frames — no frames sent)...\n",
               (unsigned)secs);
    }
    static lxveos_wifi_client_t cs[48];
    size_t found = 0;
    uint32_t beacons = 0;
    esp_err_t e = lxveos_wifi_sta_scan(secs, channel, cs, sizeof(cs) / sizeof(cs[0]), &found, &beacons);
    if (e != ESP_OK) {
        printf("station scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("  %-17s %-17s %-18s %5s %s\n", "CLIENT", "AP", "ESSID", "FRMS", "RSSI");
    for (size_t i = 0; i < found; i++) {
        printf("  %02x:%02x:%02x:%02x:%02x:%02x %02x:%02x:%02x:%02x:%02x:%02x %-18s %5u %4ddB\n",
               cs[i].sta[0], cs[i].sta[1], cs[i].sta[2], cs[i].sta[3], cs[i].sta[4], cs[i].sta[5],
               cs[i].ap[0], cs[i].ap[1], cs[i].ap[2], cs[i].ap[3], cs[i].ap[4], cs[i].ap[5],
               cs[i].essid[0] ? cs[i].essid : "<unknown>", (unsigned)cs[i].frames, cs[i].rssi);
    }
    if (lxveos_evt_enabled()) {
        for (size_t i = 0; i < found; i++) {
            char line[192];
            size_t n = lxveos_evt_begin(line, sizeof(line), "sta");
            n = lxveos_evt_kv_mac(line, sizeof(line), n, "mac", cs[i].sta);
            n = lxveos_evt_kv_mac(line, sizeof(line), n, "ap", cs[i].ap);
            n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", cs[i].rssi);
            n = lxveos_evt_kv_uint(line, sizeof(line), n, "frames", cs[i].frames);
            n = lxveos_evt_kv_hex(line, sizeof(line), n, "essid",
                                  (const uint8_t *)cs[i].essid, strlen(cs[i].essid));
            printf("%s\n", line);
        }
        char done[64];
        size_t dn = lxveos_evt_begin(done, sizeof(done), "done");
        dn = lxveos_evt_kv(done, sizeof(done), dn, "of", "stations");
        dn = lxveos_evt_kv_uint(done, sizeof(done), dn, "n", (unsigned long)found);
        printf("%s\n", done);
    }
    printf("%u client(s) inferred (%u beacons seen)\n", (unsigned)found, (unsigned)beacons);
    return 0;
}

// `probes [seconds] [channel]` — passive probe-request SSID logger (the `wifi_probe_scan` catalog op). Lists
// the SSIDs nearby client devices are actively probing for — each directed probe names a network the device
// has connected to before, so this reveals saved-network history (a recon signal + a privacy leak). Listens
// only — never sends a probe response. WIFI-capability gated.
static int cmd_probes(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot scan probes\n");
        return 0;
    }
    uint32_t secs = 12;
    uint8_t channel = 0;  // 0 = hop all channels
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 120) {
            secs = (uint32_t)v;
        } else {
            printf("usage: probes [seconds 1-120] [channel 1-13]  (default 12s, all channels)\n");
            return 0;
        }
    }
    if (argc >= 3) {
        long c = strtol(argv[2], NULL, 10);
        if (c >= 1 && c <= 13) {
            channel = (uint8_t)c;
        } else {
            printf("usage: probes [seconds 1-120] [channel 1-13]  (default 12s, all channels)\n");
            return 0;
        }
    }
    if (channel) {
        printf("passive probe-request scan for %us on channel %u (listen only — sends nothing)...\n",
               (unsigned)secs, channel);
    } else {
        printf("passive probe-request scan for %us, all channels (listen only — sends nothing)...\n",
               (unsigned)secs);
    }
    static lxveos_wifi_probe_t pr[48];
    size_t found = 0;
    uint32_t total = 0, wildcard = 0;
    esp_err_t e = lxveos_wifi_probe_scan(secs, channel, pr, sizeof(pr) / sizeof(pr[0]), &found, &total,
                                         &wildcard);
    if (e != ESP_OK) {
        printf("probe scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("  %-32s %5s %s\n", "SSID (device is hunting for)", "SEEN", "RSSI");
    for (size_t i = 0; i < found; i++) {
        printf("  ");
        // Sanitize the SSID: replace control/non-printable bytes with '.' so a crafted SSID can't garble the
        // console; pad to a fixed width for the column.
        int w = 0;
        for (const char *s = pr[i].ssid; *s && w < 32; s++, w++) {
            unsigned char c = (unsigned char)*s;
            putchar((c < 0x20 || c == 0x7f) ? '.' : (int)c);
        }
        for (; w < 32; w++) {
            putchar(' ');
        }
        printf(" %5u %4ddB\n", (unsigned)pr[i].count, pr[i].rssi);
    }
    if (lxveos_evt_enabled()) {
        for (size_t i = 0; i < found; i++) {
            char line[160];
            size_t n = lxveos_evt_begin(line, sizeof(line), "probe");
            n = lxveos_evt_kv_hex(line, sizeof(line), n, "ssid",
                                  (const uint8_t *)pr[i].ssid, strlen(pr[i].ssid));
            n = lxveos_evt_kv_uint(line, sizeof(line), n, "seen", pr[i].count);
            n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", pr[i].rssi);
            printf("%s\n", line);
        }
        char done[64];
        size_t dn = lxveos_evt_begin(done, sizeof(done), "done");
        dn = lxveos_evt_kv(done, sizeof(done), dn, "of", "probes");
        dn = lxveos_evt_kv_uint(done, sizeof(done), dn, "n", (unsigned long)found);
        printf("%s\n", done);
    }
    printf("%u directed SSID(s) from %u probe request(s) (%u wildcard/broadcast)\n",
           (unsigned)found, (unsigned)total, (unsigned)wildcard);
    if (found == 0) {
        printf("(no directed probe requests this window — nearby devices sent only wildcard probes or none)\n");
    }
    return 0;
}

// `defend [seconds]` — passive deauth/disassoc detector (the `deauth_detect` catalog operation). Counts
// deauthentication/disassociation frames — the fingerprint of a deauth attack or a rogue AP kicking
// clients — and names the busiest transmitter. Listens only; sends nothing.
static int cmd_defend(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot watch\n");
        return 0;
    }
    uint32_t secs = 15;
    uint8_t channel = 0;  // 0 = hop all channels
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 120) {
            secs = (uint32_t)v;
        } else {
            printf("usage: defend [seconds 1-120] [channel 1-13]  (default 15s, all channels)\n");
            return 0;
        }
    }
    if (argc >= 3) {
        long c = strtol(argv[2], NULL, 10);
        if (c >= 1 && c <= 13) {
            channel = (uint8_t)c;
        } else {
            printf("usage: defend [seconds 1-120] [channel 1-13]  (default 15s, all channels)\n");
            return 0;
        }
    }
    if (channel) {
        printf("passive deauth/disassoc watch for %us on channel %u (listen only — sends nothing)...\n",
               (unsigned)secs, channel);
    } else {
        printf("passive deauth/disassoc watch for %us, all channels (listen only — sends nothing)...\n",
               (unsigned)secs);
    }
    lxveos_wifi_deauth_stats_t st;
    esp_err_t e = lxveos_wifi_deauth_watch(secs, channel, &st);
    if (e != ESP_OK) {
        printf("watch failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("beacons %u — deauth %u, disassoc %u  (%u channel dwells)\n",
           (unsigned)st.beacons, (unsigned)st.deauth, (unsigned)st.disassoc,
           (unsigned)st.channels_swept);
    uint32_t hits = st.deauth + st.disassoc;
    if (hits == 0) {
        printf("verdict: clear — no deauth/disassoc activity seen\n");
    } else {
        printf("busiest source %02x:%02x:%02x:%02x:%02x:%02x (%u frames)\n",
               st.top_bssid[0], st.top_bssid[1], st.top_bssid[2],
               st.top_bssid[3], st.top_bssid[4], st.top_bssid[5], (unsigned)st.top_count);
        printf("verdict: %s\n", hits >= 20
               ? "⚠ elevated deauth/disassoc — possible attack or aggressive AP"
               : "some deauth/disassoc seen (normal at low rates; watch the busiest source)");
    }
    if (lxveos_evt_enabled() && hits > 0) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "deauth");
        n = lxveos_evt_kv_mac(line, sizeof(line), n, "bssid", st.top_bssid);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", hits);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "deauth", st.deauth);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "disassoc", st.disassoc);
        printf("%s\n", line);
    }
    return 0;
}

// `pwnwatch [seconds] [channel]` — passive Pwnagotchi-presence detector (the `pwnagotchi_detect` catalog op).
// Flags beacons from the fixed Pwnagotchi grid MAC de:ad:be:ef:de:ad and decodes the JSON identity (name +
// handshake count) from the beacon SSID. Ported from ESP32 Marauder's "Detect Pwnagotchi" (see CREDITS.md).
// Listen only — sends nothing. The decoded name is device-supplied, so it goes through sanitize_copy() before
// print (a crafted name must not carry terminal escapes into the operator's console).
static int cmd_pwnwatch(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot watch\n");
        return 0;
    }
    uint32_t secs = 15;
    uint8_t channel = 0;  // 0 = hop all channels
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 120) {
            secs = (uint32_t)v;
        } else {
            printf("usage: pwnwatch [seconds 1-120] [channel 1-13]  (default 15s, all channels)\n");
            return 0;
        }
    }
    if (argc >= 3) {
        long c = strtol(argv[2], NULL, 10);
        if (c >= 1 && c <= 13) {
            channel = (uint8_t)c;
        } else {
            printf("usage: pwnwatch [seconds 1-120] [channel 1-13]  (default 15s, all channels)\n");
            return 0;
        }
    }
    if (channel) {
        printf("passive pwnagotchi watch for %us on channel %u (listen only — sends nothing)...\n",
               (unsigned)secs, channel);
    } else {
        printf("passive pwnagotchi watch for %us, all channels (listen only — sends nothing)...\n",
               (unsigned)secs);
    }
    lxveos_wifi_pwnagotchi_stats_t st;
    esp_err_t e = lxveos_wifi_pwnagotchi_watch(secs, channel, &st);
    if (e != ESP_OK) {
        printf("watch failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("beacons %u — pwnagotchi beacons %u  (%u channel dwells)\n",
           (unsigned)st.beacons, (unsigned)st.pwnagotchi, (unsigned)st.channels_swept);
    if (st.pwnagotchi == 0) {
        printf("verdict: clear — no Pwnagotchi presence seen\n");
    } else {
        char nm[40];
        sanitize_copy(nm, sizeof(nm), st.found ? st.last_name : "");  // device-supplied name -> console-safe
        printf("⚠ Pwnagotchi present — last id \"%s\" (handshakes %u, rssi %ddBm)\n",
               nm, (unsigned)st.last_pwnd_tot, (int)st.last_rssi);
    }
    if (lxveos_evt_enabled() && st.pwnagotchi > 0) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "pwnagotchi");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", st.pwnagotchi);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "handshakes", st.last_pwnd_tot);
        printf("%s\n", line);
    }
    return 0;
}

// `eviltwin` — passive evil-twin / rogue-AP detector (a CUSTOM LxveOS op). Runs one AP scan and flags any
// ESSID advertised by more than one BSSID, or by both an open and an encrypted BSSID — the classic
// karma/Wi-Fi-Pineapple signature of a cloned network. Purely analytic over the scan; sends nothing.
static int cmd_eviltwin(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot scan\n");
        return 0;
    }
    printf("passive evil-twin / rogue-AP scan (listen only — no frames sent)...\n");
    static lxveos_wifi_ap_t aps[64];
    size_t found = 0;
    esp_err_t e = lxveos_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);
    if (e != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    int flagged = 0;
    for (size_t i = 0; i < found; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue;  // hidden SSID — nothing to compare by name
        }
        bool first = true;  // report each distinct ESSID once (at its first occurrence)
        for (size_t k = 0; k < i; k++) {
            if (strcmp(aps[k].ssid, aps[i].ssid) == 0) {
                first = false;
                break;
            }
        }
        if (!first) {
            continue;
        }
        int nbssid = 0, nopen = 0, nenc = 0;
        for (size_t j = 0; j < found; j++) {
            if (strcmp(aps[j].ssid, aps[i].ssid) == 0) {
                nbssid++;
                if (lxveos_wifi_is_open(aps[j].authmode)) {
                    nopen++;
                } else {
                    nenc++;
                }
            }
        }
        if (nbssid >= 2 || (nopen > 0 && nenc > 0)) {
            flagged++;
            char ss[33];
            sanitize_copy(ss, sizeof(ss), aps[i].ssid);  // device-supplied SSID -> console-safe
            printf("  [!] \"%s\": %d BSSIDs (%d open, %d encrypted)%s\n", ss, nbssid,
                   nopen, nenc, (nopen > 0 && nenc > 0) ? "  <- open+encrypted twin" : "");
            for (size_t j = 0; j < found; j++) {
                if (strcmp(aps[j].ssid, aps[i].ssid) == 0) {
                    printf("        %02x:%02x:%02x:%02x:%02x:%02x  ch%-2u %-6s %ddB\n",
                           aps[j].bssid[0], aps[j].bssid[1], aps[j].bssid[2],
                           aps[j].bssid[3], aps[j].bssid[4], aps[j].bssid[5],
                           aps[j].channel, lxveos_wifi_authmode_str(aps[j].authmode), aps[j].rssi);
                }
            }
            if (lxveos_evt_enabled()) {
                char line[160];
                size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
                n = lxveos_evt_kv(line, sizeof(line), n, "kind", "eviltwin");
                n = lxveos_evt_kv_hex(line, sizeof(line), n, "ssid",
                                      (const uint8_t *)aps[i].ssid, strlen(aps[i].ssid));
                n = lxveos_evt_kv_int(line, sizeof(line), n, "bssids", nbssid);
                n = lxveos_evt_kv_int(line, sizeof(line), n, "open", nopen);
                n = lxveos_evt_kv_int(line, sizeof(line), n, "enc", nenc);
                printf("%s\n", line);
            }
        }
    }
    printf("%u AP(s) scanned; %d ESSID(s) flagged\n", (unsigned)found, flagged);
    if (flagged == 0) {
        printf("verdict: clear — no duplicate-BSSID or open/encrypted-twin ESSIDs\n");
    } else {
        printf("verdict: review flagged ESSIDs (can be legit multi-AP/mesh, or a cloned rogue AP)\n");
    }
    return 0;
}

// `apaudit` — passive Wi-Fi AP security-posture auditor (the `wifi_security_audit` catalog op, a CUSTOM
// defense feature). Runs one passive AP scan and grades each network's encryption, flagging the weak ones —
// OPEN (no encryption), WEP (broken cipher) and legacy WPA (deprecated TKIP) — AND any AP advertising WPS
// (Wi-Fi Protected Setup, whose PIN is brute-forceable to recover the PSK even on WPA2/WPA3), plus a
// per-grade tally with WPS + hidden-SSID counts. Purely analytic over the scan; sends nothing.
static int cmd_apaudit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot audit\n");
        return 0;
    }
    printf("passive Wi-Fi AP security audit (listen only — no frames sent)...\n");
    static lxveos_wifi_ap_t aps[64];
    size_t found = 0;
    esp_err_t e = lxveos_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);
    if (e != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    int grade_n[6] = {0};
    int hidden = 0, weak_n = 0, wps_n = 0, flagged = 0;
    for (size_t i = 0; i < found; i++) {
        const char *note = NULL;
        int g = lxveos_wifi_auth_grade(aps[i].authmode, &note);
        if (g >= 0 && g <= 5) {
            grade_n[g]++;
        }
        if (aps[i].ssid[0] == '\0') {
            hidden++;
        }
        bool weak = (g <= 2);  // OPEN / WEP / legacy WPA — the weak encryption grades
        if (weak) {
            weak_n++;
        }
        if (aps[i].wps) {
            wps_n++;
        }
        // Flag any AP that is weak-encryption OR advertises WPS. WPS is orthogonal to the cipher: a fully
        // WPA2/WPA3 AP with WPS on is still exposed to a WPS-PIN brute force that recovers the PSK.
        if (weak || aps[i].wps) {
            flagged++;
            printf(weak ? "  [!] " : "  [W] ");  // [!] weak encryption · [W] WPS-only (encrypted, WPS on)
            // Sanitize the SSID against control bytes so a crafted name can't garble the console.
            if (aps[i].ssid[0] == '\0') {
                printf("<hidden>");
            } else {
                for (const char *s = aps[i].ssid; *s; s++) {
                    unsigned char c = (unsigned char)*s;
                    putchar((c < 0x20 || c == 0x7f) ? '.' : (int)c);
                }
            }
            printf("  %02x:%02x:%02x:%02x:%02x:%02x ch%-2u %ddB — ",
                   aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2], aps[i].bssid[3], aps[i].bssid[4],
                   aps[i].bssid[5], aps[i].channel, aps[i].rssi);
            if (weak) {
                printf("%s", note);
                if (aps[i].wps) {
                    printf(" + WPS");
                }
            } else {
                printf("WPS enabled on %s — WPS-PIN attack surface", note);
            }
            printf("\n");
            if (lxveos_evt_enabled()) {
                char line[160];
                size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
                n = lxveos_evt_kv(line, sizeof(line), n, "kind", weak ? "weak" : "wps");
                n = lxveos_evt_kv_mac(line, sizeof(line), n, "bssid", aps[i].bssid);
                n = lxveos_evt_kv_hex(line, sizeof(line), n, "ssid",
                                      (const uint8_t *)aps[i].ssid, strlen(aps[i].ssid));
                n = lxveos_evt_kv_int(line, sizeof(line), n, "grade", g);
                if (aps[i].wps) {
                    n = lxveos_evt_kv_int(line, sizeof(line), n, "wps", 1);
                }
                printf("%s\n", line);
            }
        }
    }
    printf("%u AP(s) scanned — open %d, WEP %d, WPA %d, WPA2 %d, WPA3 %d, other %d (%d hidden SSID, %d WPS)\n",
           (unsigned)found, grade_n[0], grade_n[1], grade_n[2], grade_n[3], grade_n[4], grade_n[5], hidden,
           wps_n);
    if (flagged == 0) {
        printf("verdict: clear — every AP in range uses WPA2 or better and none advertise WPS\n");
    } else {
        printf("verdict: ⚠ %d flagged AP(s) — ", flagged);
        if (weak_n) {
            printf("%d weak-encryption (open/WEP/legacy-WPA are eavesdroppable or crackable)%s",
                   weak_n, wps_n ? "; " : "");
        }
        if (wps_n) {
            printf("%d WPS-enabled (WPS-PIN brute-force recovers the PSK regardless of its strength)", wps_n);
        }
        printf(" — prefer WPA2/WPA3 with WPS disabled\n");
    }
    return 0;
}

// `blescan [seconds]` — passive BLE device scan (the `ble_scan` catalog operation). Runs a NimBLE GAP
// discovery in PASSIVE mode (the controller never sends a SCAN_REQ) and lists nearby advertisers with
// address, address type, RSSI, GAP flags and local name. This scan is passive (never advertises, never
// sends a SCAN_REQ). The non-connectable BLE broadcaster role is compiled out, so the build cannot emit a
// broadcast advert flood; a connectable keyboard advert exists only under the arm-gated `badble` op. Gated
// on the BLE capability.
static int cmd_blescan(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: blescan [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("passive BLE scan for %us (GAP observe — no scan requests, advertises nothing)...\n",
           (unsigned)secs);
    static lxveos_ble_dev_t devs[48];
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, sizeof(devs) / sizeof(devs[0]), &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("  %-17s %-7s %5s %-5s %-9s %s\n", "ADDRESS", "TYPE", "RSSI", "FLAGS", "VENDOR", "NAME");
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        char flags[6];
        if (d->flags_present) {
            snprintf(flags, sizeof(flags), "0x%02x", d->adv_flags);
        } else {
            flags[0] = '-';
            flags[1] = '\0';
        }
        char vendor[12];
        const char *cn = d->has_mfg ? lxveos_ble_company_name(d->company_id) : NULL;
        if (cn != NULL) {
            snprintf(vendor, sizeof(vendor), "%s", cn);
        } else if (d->fastpair) {
            snprintf(vendor, sizeof(vendor), "FastPair");
        } else if (d->has_mfg) {
            snprintf(vendor, sizeof(vendor), "0x%04x", d->company_id);
        } else {
            vendor[0] = '-';
            vendor[1] = '\0';
        }
        // Identity cell = the local name and/or the GAP appearance (a device with no name but a known
        // appearance still gets a useful "[Watch]" / "[Earbuds]" label).
        char appr[12];
        if (d->appearance_present) {
            lxveos_ble_appearance_str(d->appearance, appr, sizeof(appr));
        } else {
            appr[0] = '\0';
        }
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name);  // control-byte-safe copy of the device-supplied name
        char ident[48];
        if (d->name_len && appr[0]) {
            snprintf(ident, sizeof(ident), "%s [%s]", nm, appr);
        } else if (d->name_len) {
            snprintf(ident, sizeof(ident), "%s", nm);
        } else if (appr[0]) {
            snprintf(ident, sizeof(ident), "[%s]", appr);
        } else {
            ident[0] = '\0';
        }
        printf("  %02x:%02x:%02x:%02x:%02x:%02x %-7s %4ddB %-5s %-9s %s\n",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0],
               lxveos_ble_addr_type_str(d->addr_type), d->rssi, flags, vendor, ident);
        // Advertised 16-bit service-class UUIDs get their own wrapped line so the main table stays narrow.
        // Each is named when known (Battery / HeartRate / FastPair / …), else shown as raw 0xNNNN.
        if (d->svc_uuid_count > 0) {
            printf("        svc:");
            for (uint8_t u = 0; u < d->svc_uuid_count; u++) {
                const char *sn = lxveos_ble_service_name(d->svc_uuids[u]);
                if (sn != NULL) {
                    printf(" %s", sn);
                } else {
                    printf(" 0x%04x", d->svc_uuids[u]);
                }
            }
            if (d->svc_uuids_partial) {
                printf(" (+more)");
            }
            printf("\n");
        }
    }
    if (lxveos_evt_enabled()) {
        for (size_t i = 0; i < found; i++) {
            const lxveos_ble_dev_t *d = &devs[i];
            // addr[] is little-endian (controller order); reverse it to the MSB-first MAC the table shows.
            uint8_t a[6] = {d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0]};
            char line[224];
            size_t n = lxveos_evt_begin(line, sizeof(line), "ble");
            n = lxveos_evt_kv_mac(line, sizeof(line), n, "addr", a);
            n = lxveos_evt_kv(line, sizeof(line), n, "type", lxveos_ble_addr_type_str(d->addr_type));
            n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", d->rssi);
            if (d->name_len) {
                n = lxveos_evt_kv_hex(line, sizeof(line), n, "name", (const uint8_t *)d->name, d->name_len);
            }
            if (d->has_mfg) {
                // numeric company ID (not the name — company names can contain spaces); CC maps it.
                n = lxveos_evt_kv_uint(line, sizeof(line), n, "company", d->company_id);
            }
            if (d->fastpair) {
                n = lxveos_evt_kv_int(line, sizeof(line), n, "fp", 1);
            }
            if (d->appearance_present) {
                n = lxveos_evt_kv_uint(line, sizeof(line), n, "appr", d->appearance);
            }
            if (d->tracker) {
                n = lxveos_evt_kv_uint(line, sizeof(line), n, "tracker", d->tracker);
            }
            printf("%s\n", line);
        }
        char done[64];
        size_t dn = lxveos_evt_begin(done, sizeof(done), "done");
        dn = lxveos_evt_kv(done, sizeof(done), dn, "of", "blescan");
        dn = lxveos_evt_kv_uint(done, sizeof(done), dn, "n", (unsigned long)found);
        printf("%s\n", done);
    }
    printf("%u BLE device(s) in range\n", (unsigned)found);
    return 0;
}

// `bleflood [seconds]` — passive BLE advertisement-flood / spam DETECTOR (the `ble_flood_detect` catalog
// op, a CUSTOM defense feature). Runs one passive GAP observe and measures advertiser churn: a BLE-spam /
// advert-flood attack (Flipper "BLE Spam", Apple/Android/Windows popup floods) rotates through a torrent of
// distinct — usually random — addresses, while a normal room shows a small, stable set. Listen only — sends
// nothing, advertises nothing. BLE-capability gated.
static int cmd_bleflood(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot watch\n");
        return 0;
    }
    uint32_t secs = 8;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: bleflood [seconds 1-60]  (default 8)\n");
            return 0;
        }
    }
    printf("passive BLE advert-flood/spam detector for %us (GAP observe — advertises nothing)...\n",
           (unsigned)secs);
    lxveos_ble_flood_stats_t st;
    esp_err_t e = lxveos_ble_flood_watch(secs, &st);
    if (e != ESP_OK) {
        printf("bleflood failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    uint32_t per_sec = st.seconds ? st.unique_addrs / st.seconds : st.unique_addrs;
    printf("adverts %u — unique advertisers %u%s (%u random) — ~%u new/s over %us\n",
           (unsigned)st.total_adv, (unsigned)st.unique_addrs,
           st.unique_overflow ? "+ (table full)" : "", (unsigned)st.random_addrs,
           (unsigned)per_sec, (unsigned)st.seconds);
    if (st.top_count > 0) {
        printf("busiest advertiser %02x:%02x:%02x:%02x:%02x:%02x (%s) — %u adverts\n",
               st.top_addr[5], st.top_addr[4], st.top_addr[3], st.top_addr[2], st.top_addr[1],
               st.top_addr[0], lxveos_ble_addr_type_str(st.top_addr_type), (unsigned)st.top_count);
    }
    // Vendor breakdown of the advert payloads — a BLE-spam flood's payloads cluster on one vendor, so the
    // dominant spam-capable vendor (Apple/Microsoft/Google[+FastPair]/Samsung) attributes the attack.
    uint32_t vsum = st.v_apple + st.v_microsoft + st.v_google + st.v_samsung + st.v_fastpair + st.v_other_mfg;
    if (vsum > 0) {
        printf("vendors (adverts): Apple %u, Microsoft %u, Google %u, Samsung %u, FastPair %u, other %u\n",
               (unsigned)st.v_apple, (unsigned)st.v_microsoft, (unsigned)st.v_google,
               (unsigned)st.v_samsung, (unsigned)st.v_fastpair, (unsigned)st.v_other_mfg);
    }
    struct {
        const char *name;
        uint32_t    n;
    } vend[] = {
        {"Apple", st.v_apple},
        {"Microsoft", st.v_microsoft},
        {"Google", st.v_google + st.v_fastpair},
        {"Samsung", st.v_samsung},
    };
    const char *dom = NULL;
    uint32_t    domn = 0;
    for (size_t i = 0; i < sizeof(vend) / sizeof(vend[0]); i++) {
        if (vend[i].n > domn) {
            domn = vend[i].n;
            dom  = vend[i].name;
        }
    }
    // A normal room shows a small, stable advertiser set; a flood churns through many rotating addresses.
    bool flood = st.unique_overflow || st.unique_addrs >= 80 || per_sec >= 12;
    if (flood) {
        printf("verdict: ⚠ possible BLE advertisement flood/spam — high advertiser churn "
               "(many rotating addresses%s)", st.unique_overflow ? ", tracking table saturated" : "");
        if (dom != NULL) {
            printf(" — dominant payload vendor: %s (BLE-spam signature)", dom);
        }
        printf("\n");
    } else {
        printf("verdict: clear — no BLE advertisement flood (advertiser churn normal)\n");
    }
    if (lxveos_evt_enabled() && flood) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "bleflood");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "rate", per_sec);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "uniq", st.unique_addrs);
        if (dom != NULL) {
            n = lxveos_evt_kv(line, sizeof(line), n, "vendor", dom);
        }
        printf("%s\n", line);
    }
    return 0;
}

// `btracker [seconds]` — passive BLE item-tracker / stalking detector (the `ble_tracker_detect` catalog op,
// a CUSTOM defense feature). Runs one passive GAP observe and flags advertisers whose payload matches a
// known item-tracker signature — Apple Find My/AirTag (mfg type 0x12), Tile (0xFEED), Samsung SmartTag
// (0xFD5A), Chipolo (0xFE33), PebbleBee (0xFA25), Google Find My Network (0xFEAA svc-data frame 0x40, not
// Eddystone) — all verified against the AirGuard anti-stalking project. A tracker that travels with you but
// is NOT yours is the AirTag-stalking signal; re-run over time and watch for one that persistently follows.
// Listen only — advertises nothing. BLE-capability gated.
static int cmd_btracker(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan for trackers\n");
        return 0;
    }
    uint32_t secs = 8;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: btracker [seconds 1-60]  (default 8)\n");
            return 0;
        }
    }
    printf("passive BLE item-tracker scan for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    static lxveos_ble_dev_t devs[48];
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, sizeof(devs) / sizeof(devs[0]), &found);
    if (e != ESP_OK) {
        printf("btracker scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    int trackers = 0;
    printf("  %-17s %-7s %5s %-14s %s\n", "ADDRESS", "TYPE", "RSSI", "TRACKER", "NAME");
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        const char *tn = lxveos_ble_tracker_str(d->tracker);
        if (tn == NULL) {
            continue;  // not a known tracker
        }
        trackers++;
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name_len ? d->name : "");  // device-supplied name -> console-safe
        printf("  %02x:%02x:%02x:%02x:%02x:%02x %-7s %4ddB %-14s %s\n",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0],
               lxveos_ble_addr_type_str(d->addr_type), d->rssi, tn, nm);
        if (lxveos_evt_enabled()) {
            uint8_t a[6] = {d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0]};
            char line[160];
            size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
            n = lxveos_evt_kv(line, sizeof(line), n, "kind", "tracker");
            n = lxveos_evt_kv_mac(line, sizeof(line), n, "addr", a);
            n = lxveos_evt_kv(line, sizeof(line), n, "vendor", tn);
            n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", d->rssi);
            if (d->name_len) {
                n = lxveos_evt_kv_hex(line, sizeof(line), n, "name", (const uint8_t *)d->name, d->name_len);
            }
            printf("%s\n", line);
        }
    }
    printf("%d tracker(s) among %u BLE device(s) in range\n", trackers, (unsigned)found);
    if (trackers == 0) {
        printf("verdict: clear — no known item-trackers advertising nearby\n");
    } else {
        printf("verdict: %d tracker(s) present. A tracker you don't own that FOLLOWS you over time/place is "
               "the stalking signal — re-run `btracker` as you move and watch for one that persists.\n",
               trackers);
    }
    return 0;
}

// `wardrive` — passive Wi-Fi wardrive CSV export (the `wifi_wardrive` catalog op). Runs one passive AP scan
// and prints a machine-importable CSV — one row per AP: bssid,ssid,channel,rssi,auth,hidden — for a host
// mapping/inventory tool to ingest. The unit has no GPS, so coordinates are the host's to add (pair the
// export with the host's location). Listen only — transmits nothing. WIFI-capability gated.
static int cmd_wardrive(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot wardrive\n");
        return 0;
    }
    printf("passive Wi-Fi wardrive scan (listen only — no frames sent)...\n");
    static lxveos_wifi_ap_t aps[64];
    size_t found = 0;
    esp_err_t e = lxveos_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);
    if (e != ESP_OK) {
        printf("wardrive scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    // CSV (RFC4180-ish): the SSID is quoted with embedded quotes doubled and control bytes sanitized, so a
    // comma/quote in a network name can't break the row. GPS-less by design — the host supplies coordinates.
    printf("bssid,ssid,channel,rssi,auth,hidden\n");
    for (size_t i = 0; i < found; i++) {
        // The SSID is the only field that can contain a comma/quote/control byte; csv_quote_field emits a
        // safe RFC4180 double-quoted field (embedded quotes doubled, control bytes -> '.'). Worst case is a
        // 32-char SSID of all quotes -> 64 + 2 quotes + NUL, so 80 is always enough.
        char q[80];
        csv_quote_field(q, sizeof(q), aps[i].ssid);
        printf("%02x:%02x:%02x:%02x:%02x:%02x,%s,%u,%d,%s,%d\n",
               aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2], aps[i].bssid[3], aps[i].bssid[4],
               aps[i].bssid[5], q, aps[i].channel, aps[i].rssi,
               lxveos_wifi_authmode_str(aps[i].authmode), aps[i].ssid[0] ? 0 : 1);
    }
    printf("# %u AP(s) exported (GPS-less — host adds coordinates)\n", (unsigned)found);
    return 0;
}

// Shared passive-BLE scan result buffer for the BLE commands added here (blewardrive / flipper / future BLE
// detectors). The CLI is single-threaded and these never run concurrently, so one buffer serves all of them —
// which keeps .dram0.bss small on the no-PSRAM boards, where DRAM is the tight budget (the CYD 3.5" is right at
// the limit). Older BLE commands keep their own buffers; new ones share this.
#define LXVEOS_BLE_SCAN_MAX 48
static lxveos_ble_dev_t s_ble_scan[LXVEOS_BLE_SCAN_MAX];

// `blewardrive [seconds]` — passive BLE wardrive CSV export (the `ble_wardrive` catalog op). One passive
// GAP-observe scan -> a machine-importable `addr,addr_type,name,rssi,vendor,tracker` CSV, mirroring the Wi-Fi
// `wardrive` op over BLE. Ported from ESP32 Marauder's BLE wardrive (see CREDITS.md). GPS-less by design (the
// host adds coordinates). Listen only — advertises nothing, sends no scan requests.
static int cmd_blewardrive(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot wardrive\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: blewardrive [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("passive BLE wardrive scan for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    lxveos_ble_dev_t *devs = s_ble_scan;   // shared buffer (single-threaded CLI) — keeps DRAM small
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, LXVEOS_BLE_SCAN_MAX, &found);
    if (e != ESP_OK) {
        printf("ble wardrive scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    // CSV (RFC4180-ish): only the device-supplied name can carry a comma/quote/control byte, so it goes
    // through csv_quote_field; the other fields are fixed labels / numbers. GPS-less — host adds coordinates.
    printf("addr,addr_type,name,rssi,vendor,tracker\n");
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        char q[80];
        csv_quote_field(q, sizeof(q), d->name);
        const char *cn = d->has_mfg ? lxveos_ble_company_name(d->company_id) : NULL;
        char vendor[16];
        if (cn != NULL) {
            snprintf(vendor, sizeof(vendor), "%s", cn);
        } else if (d->has_mfg) {
            snprintf(vendor, sizeof(vendor), "0x%04x", d->company_id);
        } else {
            vendor[0] = '-';
            vendor[1] = '\0';
        }
        const char *tr = lxveos_ble_tracker_str(d->tracker);
        // BLE addresses are little-endian in the struct; print MSB-first (the conventional display order).
        printf("%02x:%02x:%02x:%02x:%02x:%02x,%s,%s,%d,%s,%s\n",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0],
               lxveos_ble_addr_type_str(d->addr_type), q, d->rssi, vendor, tr ? tr : "-");
    }
    printf("# %u device(s) exported (GPS-less — host adds coordinates)\n", (unsigned)found);
    return 0;
}

// `flipper [seconds]` — passive Flipper Zero detector (the `flipper_detect` catalog op). One passive BLE scan,
// flags advertisers carrying a Flipper service UUID (0x3081/82/83) and names the case colour. Ported from
// ESP32 Marauder "Flipper Sniff" (see CREDITS.md). Listen only — advertises nothing, sends no scan requests.
static int cmd_flipper(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: flipper [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("passive Flipper Zero scan for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    lxveos_ble_dev_t *devs = s_ble_scan;   // shared buffer (single-threaded CLI) — keeps DRAM small
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, LXVEOS_BLE_SCAN_MAX, &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    unsigned hits = 0;
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        const char *color = lxveos_ble_flipper_color(d);
        if (color == NULL) {
            continue;
        }
        hits++;
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name);   // device-supplied name -> console-safe
        printf("  flipper(%s) %02x:%02x:%02x:%02x:%02x:%02x  rssi %ddBm  %s\n",
               color, d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0], d->rssi, nm);
    }
    if (hits) {
        printf("verdict: ⚠ %u Flipper Zero(s) present (%u device(s) scanned)\n", hits, (unsigned)found);
    } else {
        printf("verdict: clear — no Flipper Zero seen (%u device(s) scanned)\n", (unsigned)found);
    }
    if (lxveos_evt_enabled() && hits > 0) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "flipper");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", hits);
        printf("%s\n", line);
    }
    return 0;
}

// `meta [seconds]` — passive Meta / Ray-Ban Meta glasses + Oculus detector (the `meta_detect` catalog op). One
// passive BLE scan, flags advertisers whose mfg company ID or a service UUID is in the Meta set (a deny-list
// strips Apple/Samsung/Microsoft popup-flood payloads first). Ported from ESP32 Marauder "Meta Detect" (see
// CREDITS.md). Listen only — advertises nothing, sends no scan requests.
static int cmd_meta(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: meta [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("passive Meta/Ray-Ban glasses scan for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    lxveos_ble_dev_t *devs = s_ble_scan;   // shared buffer (single-threaded CLI) — keeps DRAM small
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, LXVEOS_BLE_SCAN_MAX, &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    unsigned hits = 0;
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        if (!lxveos_ble_is_meta(d)) {
            continue;
        }
        hits++;
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name);   // device-supplied name -> console-safe
        printf("  meta %02x:%02x:%02x:%02x:%02x:%02x  rssi %ddBm  %s\n",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0], d->rssi, nm);
    }
    if (hits) {
        printf("verdict: ⚠ %u Meta/Ray-Ban identifier(s) seen (%u device(s) scanned)\n", hits, (unsigned)found);
    } else {
        printf("verdict: clear — no Meta device seen (%u device(s) scanned)\n", (unsigned)found);
    }
    if (lxveos_evt_enabled() && hits > 0) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "meta");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", hits);
        printf("%s\n", line);
    }
    return 0;
}

// `skimmer [seconds]` — passive card-skimmer heuristic (the `skimmer_detect` catalog op). One passive BLE scan,
// flags advertisers whose local name is EXACTLY a default HC-0x BT-serial module name (HC-03/05/06) — the stock
// modules cheap skimmers reuse. Ported from ESP32 Marauder "Detect Card Skimmers" (see CREDITS.md). NARROW: also
// flags legit hobby HC-0x modules (shown as "possible"), and BLE-only misses classic-BT-only skimmers. Listen
// only — advertises nothing, sends no scan requests.
static int cmd_skimmer(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: skimmer [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("passive card-skimmer scan for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    lxveos_ble_dev_t *devs = s_ble_scan;   // shared buffer (single-threaded CLI) — keeps DRAM small
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, LXVEOS_BLE_SCAN_MAX, &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    unsigned hits = 0;
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        if (!lxveos_ble_is_skimmer(d)) {
            continue;
        }
        hits++;
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name);   // device-supplied name -> console-safe
        printf("  possible-skimmer %02x:%02x:%02x:%02x:%02x:%02x  rssi %ddBm  name=%s\n",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0], d->rssi, nm);
    }
    if (hits) {
        // Deliberately hedged: a default-named BT-serial module is only a *possible* skimmer (legit hobby
        // modules share the name; BLE-only won't see classic-BT-only skimmers). Never states it as certain.
        printf("verdict: ⚠ %u default-named BT-serial module(s) — possible skimmer, verify in person "
               "(%u device(s) scanned)\n", hits, (unsigned)found);
    } else {
        printf("verdict: clear — no default-named BT-serial module seen (%u device(s) scanned)\n",
               (unsigned)found);
    }
    if (lxveos_evt_enabled() && hits > 0) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "skimmer");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", hits);
        printf("%s\n", line);
    }
    return 0;
}

// `flock [seconds]` — passive Flock Safety camera heuristic (the `flock_detect` catalog op). One passive BLE
// scan, flags advertisers carrying Flock's XUNTONG manufacturer ID (0x09C8), tiered LIKELY (a confirming Flock
// name pattern) / POSSIBLE (nameless XUNTONG advert). Ported from ESP32 Marauder "Flock Sniff" (see CREDITS.md).
// EXPERIMENTAL — LxveOS carries only the specific XUNTONG signal, not Marauder's FP-prone broad-OUI + Wi-Fi
// SSID-substring paths, so it never asserts a confident Flock ID. Listen only — advertises nothing.
static int cmd_flock(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: flock [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("passive Flock-camera scan for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    lxveos_ble_dev_t *devs = s_ble_scan;   // shared buffer (single-threaded CLI) — keeps DRAM small
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, LXVEOS_BLE_SCAN_MAX, &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    unsigned likely = 0;
    unsigned possible = 0;
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        uint8_t conf = lxveos_ble_flock_confidence(d);
        const char *tier = lxveos_ble_flock_str(conf);
        if (tier == NULL) {
            continue;
        }
        if (conf == LXVEOS_BLE_FLOCK_LIKELY) {
            likely++;
        } else {
            possible++;
        }
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name);   // device-supplied name -> console-safe
        printf("  flock(%s) %02x:%02x:%02x:%02x:%02x:%02x  rssi %ddBm  %s\n",
               tier, d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0], d->rssi, nm);
    }
    unsigned hits = likely + possible;
    if (hits) {
        // Deliberately hedged: XUNTONG is Flock's OEM chipset but not exclusively theirs, so this is a lead to
        // verify in person, never a confident camera ID. LIKELY has a confirming name; POSSIBLE is nameless.
        printf("verdict: ⚠ %u possible Flock device(s) — %u likely / %u possible, verify in person "
               "(%u device(s) scanned)\n", hits, likely, possible, (unsigned)found);
    } else {
        printf("verdict: clear — no Flock (XUNTONG) advertiser seen (%u device(s) scanned)\n", (unsigned)found);
    }
    if (lxveos_evt_enabled() && hits > 0) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "flock");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", hits);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "likely", likely);
        printf("%s\n", line);
    }
    return 0;
}

// `surveil [seconds]` — counter-surveillance BLE sweep (the `surveil_scan` catalog op, custom). One passive scan,
// then folds EVERY passive BLE detector into a single "what surveillance gear is near me?" answer: item-trackers,
// Flock cameras, Meta / Ray-Ban glasses, Flipper Zeros, and possible card-skimmers — per-device categories plus a
// category tally. A privacy / anti-stalking sweep; listen only — advertises nothing, sends no scan requests.
static int cmd_surveil(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot scan\n");
        return 0;
    }
    uint32_t secs = 6;
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v >= 1 && v <= 60) {
            secs = (uint32_t)v;
        } else {
            printf("usage: surveil [seconds 1-60]  (default 6)\n");
            return 0;
        }
    }
    printf("counter-surveillance sweep for %us (GAP observe — advertises nothing)...\n", (unsigned)secs);
    lxveos_ble_dev_t *devs = s_ble_scan;   // shared buffer (single-threaded CLI) — keeps DRAM small
    size_t found = 0;
    esp_err_t e = lxveos_ble_scan(secs, devs, LXVEOS_BLE_SCAN_MAX, &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    // The category bits we walk, in display order. Per-category tallies parallel this list.
    static const uint8_t CATS[] = {
        LXVEOS_SURVEIL_TRACKER, LXVEOS_SURVEIL_FLOCK, LXVEOS_SURVEIL_META,
        LXVEOS_SURVEIL_FLIPPER, LXVEOS_SURVEIL_SKIMMER,
    };
    const size_t ncats = sizeof(CATS) / sizeof(CATS[0]);
    unsigned tally[sizeof(CATS) / sizeof(CATS[0])] = {0};
    unsigned hits = 0;
    for (size_t i = 0; i < found; i++) {
        const lxveos_ble_dev_t *d = &devs[i];
        uint8_t flags = lxveos_ble_surveil_flags(d);
        if (flags == LXVEOS_SURVEIL_NONE) {
            continue;
        }
        hits++;
        // Build a "cat1+cat2" label for this device and bump each matched category's tally.
        char cats[64];
        size_t cl = 0;
        cats[0] = '\0';
        for (size_t c = 0; c < ncats; c++) {
            if (flags & CATS[c]) {
                tally[c]++;
                const char *nm = lxveos_ble_surveil_str(CATS[c]);
                cl += (size_t)snprintf(cats + cl, sizeof(cats) - cl, "%s%s", cl ? "+" : "", nm ? nm : "?");
                if (cl >= sizeof(cats)) {
                    cl = sizeof(cats) - 1;   // snprintf truncated; keep cl in-bounds for the next append
                }
            }
        }
        char nm[32];
        sanitize_copy(nm, sizeof(nm), d->name);   // device-supplied name -> console-safe
        printf("  [%s] %02x:%02x:%02x:%02x:%02x:%02x  rssi %ddBm  %s\n",
               cats, d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0], d->rssi, nm);
    }
    if (hits) {
        printf("verdict: ⚠ %u surveillance-relevant device(s) — tracker:%u flock:%u meta:%u flipper:%u skimmer:%u "
               "(%u scanned). Skimmer/flock are heuristic — verify in person.\n",
               hits, tally[0], tally[1], tally[2], tally[3], tally[4], (unsigned)found);
    } else {
        printf("verdict: clear — no surveillance-relevant BLE device seen (%u scanned)\n", (unsigned)found);
    }
    if (lxveos_evt_enabled() && hits > 0) {
        char line[160];
        size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
        n = lxveos_evt_kv(line, sizeof(line), n, "kind", "surveil");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "count", hits);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "tracker", tally[0]);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "flock", tally[1]);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "meta", tally[2]);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "flipper", tally[3]);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "skimmer", tally[4]);
        printf("%s\n", line);
    }
    return 0;
}

// `arm [token]` — two-factor enable for offensive-TX ops. `arm` (no token) starts a request and prints a
// one-time confirm code; `arm <token>` confirms it within 30s. Offensive TX is compiled in by default; only
// a conservative LXVEOS_TX_DISABLE build has nothing to arm. Recon/defense ops never need arming. The gate
// itself lives in lxveos_arm; each offensive op checks lxveos_arm_can_emit() immediately before it transmits.
static int cmd_arm(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_arm_tx_compiled()) {
        printf("offensive TX is compiled OUT of this build (LXVEOS_TX_DISABLE set) — nothing to arm.\n");
        printf("recon/defense ops need no arming; they run as usual.\n");
        if (lxveos_evt_enabled()) {
            printf("LXVEOS/1 arm state=tx_disabled\n");
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        // Read-only query — never mutates arm state (unlike bare `arm`, which requests).
        printf("arm state: %s\n", lxveos_arm_state_name(lxveos_arm_state()));
        if (lxveos_evt_enabled()) {
            printf("LXVEOS/1 arm state=%s\n", lxveos_arm_state_name(lxveos_arm_state()));
        }
        return 0;
    }
    if (argc >= 2) {
        uint32_t token = (uint32_t)strtoul(argv[1], NULL, 10);
        esp_err_t e = lxveos_arm_confirm(token);
        if (e == ESP_OK) {
            printf("ARMED — offensive-TX ops permitted until 'disarm' or inactivity timeout.\n");
        } else {
            printf("arm confirm failed (%s) — state now %s. Re-run 'arm' to start over.\n",
                   esp_err_to_name(e), lxveos_arm_state_name(lxveos_arm_state()));
        }
        if (lxveos_evt_enabled()) {
            printf("LXVEOS/1 arm state=%s\n", lxveos_arm_state_name(lxveos_arm_state()));
        }
        return 0;
    }
    // No-arg `arm`: report if already ARMED instead of re-requesting — otherwise "checking" would drop a live
    // armed session back to PENDING (a footgun). Use `arm status` for a pure read-only query in any state.
    if (lxveos_arm_state() == LXVEOS_ARM_ARMED) {
        printf("already ARMED — offensive TX permitted. Run 'disarm' to return to SAFE "
               "(also auto-disarms after inactivity).\n");
        if (lxveos_evt_enabled()) {
            printf("LXVEOS/1 arm state=armed\n");
        }
        return 0;
    }
    uint32_t token = 0;
    esp_err_t e = lxveos_arm_request(&token);
    if (e != ESP_OK) {
        printf("arm request failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("arm requested. Confirm within 30s:  arm %u\n", (unsigned)token);
    if (lxveos_evt_enabled()) {
        // window=30 mirrors CONFIRM_WINDOW_US in lxveos_arm.c (and the prose above).
        printf("LXVEOS/1 arm state=pending token=%u window=30\n", (unsigned)token);
    }
    return 0;
}

// `disarm` — hard kill: return to SAFE immediately. Always available.
static int cmd_disarm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (locked()) {
        return 0;
    }
    lxveos_arm_disarm();
    printf("disarmed — state SAFE. Offensive TX not permitted.\n");
    if (lxveos_evt_enabled()) {
        printf("LXVEOS/1 arm state=safe\n");
    }
    return 0;
}

// Print callback for `evilportal creds` — one captured user/pass pair per line.
static void ep_print_cred(const char *user, const char *pass)
{
    printf("  user='%s' pass='%s'\n", user, pass);
}

// Print callback for `evilportal templates` — one template per line, marking the selected one.
static void ep_print_template(const char *id, const char *name, bool selected)
{
    printf("  %c %-10s %s\n", selected ? '*' : ' ', id, name);
}

// `evilportal [ssid|creds|stop]` — the evil_portal op: rogue OPEN AP + captive credential-capture portal.
// OFFENSIVE-TX op, so it needs arm (`agree`, then `arm` -> `arm <token>`). `evilportal stop` tears it down;
// `evilportal creds` lists the retained captures; `evilportal [ssid]` starts it (default SSID if none).
static int cmd_evilportal(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot run evil-portal\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        lxveos_evilportal_stop();
        printf("evil-portal stopped (%u credential(s) captured)\n",
               (unsigned)lxveos_evilportal_captures());
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "creds") == 0) {
        uint32_t n = lxveos_evilportal_captures();
        if (n == 0) {
            printf("no credentials captured\n");
            return 0;
        }
        printf("captured credentials (%u total, last %u shown):\n", (unsigned)n, n < 16 ? (unsigned)n : 16u);
        lxveos_evilportal_creds_each(ep_print_cred);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "templates") == 0) {
        printf("captive-portal templates (* = selected):\n");
        lxveos_evilportal_templates_each(ep_print_template);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "template") == 0) {
        if (lxveos_evilportal_template_set(argv[2])) {
            printf("template set to '%s' (applies to the next request)\n", argv[2]);
        } else {
            printf("unknown template '%s' — 'evilportal templates' to list\n", argv[2]);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "karma") == 0) {
        if (lxveos_evilportal_running()) {
            printf("evil-portal already running — 'evilportal stop' first\n");
            return 0;
        }
        if (!lxveos_arm_can_emit()) {
            printf("offensive TX not permitted — run 'arm' first (this is an offensive-TX op).\n");
            return 0;
        }
        printf("karma: listening 10s for the SSID nearby clients probe for most...\n");
        char chosen[33] = {0};
        esp_err_t e = lxveos_evilportal_start_karma(chosen, sizeof(chosen));
        if (e == ESP_OK) {
            printf("karma AP up as \"%s\" — captive login at http://192.168.4.1/ (armed)\n", chosen);
        } else if (e == ESP_ERR_NOT_FOUND) {
            printf("no directed probe requests seen — nothing to lure with. Try `probes`, or `evilportal <ssid>`.\n");
        } else {
            printf("karma start failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (lxveos_evilportal_running()) {
        printf("evil-portal already running (%u captured) — 'evilportal stop' to end\n",
               (unsigned)lxveos_evilportal_captures());
        return 0;
    }
    if (!lxveos_arm_can_emit()) {
        printf("offensive TX not permitted — run 'arm' first (this is an offensive-TX op).\n");
        return 0;
    }
    const char *ssid = (argc >= 2) ? argv[1] : "Free_WiFi";
    esp_err_t e = lxveos_evilportal_start(ssid);
    if (e == ESP_OK) {
        printf("evil-portal up: OPEN AP \"%s\" — captive login at http://192.168.4.1/  (armed)\n", ssid);
        printf("submitted credentials are logged (WARN) + counted; 'evilportal stop' to end.\n");
    } else {
        printf("evil-portal start failed: %s\n", esp_err_to_name(e));
    }
    return 0;
}

// badble — arm-gated BLE HID keystroke injection ("BadBLE"). Advertises as a keyboard; on pairing, plays a
// DuckyScript-lite script into the target host. `badble stop` / `badble status`, else the args form the
// script (commands separated by ';'). esp_console splits on spaces, so argv[1..] is rejoined with spaces —
// quote the whole script to preserve exact spacing (badble "GUI r;DELAY 400;STRING notepad;ENTER").
static int cmd_badble(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build — cannot run HID injection\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        lxveos_ble_hid_stop();
        printf("badble stopped\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        printf("badble: %s%s\n", lxveos_ble_hid_running() ? "active" : "idle",
               lxveos_ble_hid_connected() ? ", host connected" : "");
        return 0;
    }
    if (argc < 2) {
        printf("usage: badble <script>   e.g. badble \"GUI r;DELAY 400;STRING notepad;ENTER\"\n");
        printf("       commands (';'-separated): STRING <text> | DELAY <ms> | ENTER/TAB/ESC/SPACE/UP/DOWN/... |\n");
        printf("       GUI/CTRL/ALT/SHIFT/CTRL-ALT <key>. Requires 'arm' first (offensive-TX op).\n");
        return 0;
    }
    if (lxveos_ble_hid_running()) {
        printf("badble already running — 'badble stop' first\n");
        return 0;
    }
    if (!lxveos_arm_can_emit()) {
        printf("offensive TX not permitted — run 'arm' first (this is an offensive-TX op).\n");
        return 0;
    }
    // Rejoin argv[1..] into one script string (single-space separated).
    char script[256];
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        int w = snprintf(script + off, sizeof(script) - off, "%s%s", i > 1 ? " " : "", argv[i]);
        if (w < 0 || (size_t)w >= sizeof(script) - off) {
            off = sizeof(script) - 1;
            break;
        }
        off += (size_t)w;
    }
    script[off] = '\0';

    esp_err_t e = lxveos_ble_hid_inject(script);
    if (e == ESP_OK) {
        printf("badble: advertising as BLE keyboard \"LxveOS-KB\" — pair a target within 60s to inject (armed)\n");
        printf("script: %s\n", script);
    } else if (e == ESP_ERR_NOT_ALLOWED) {
        printf("offensive TX not permitted — run 'arm' first.\n");
    } else {
        printf("badble start failed: %s\n", esp_err_to_name(e));
    }
    return 0;
}

// ir — IR capture + replay (the ir_recv / ir_send ops), a universal remote via the RMT peripheral. Pins
// are operator-supplied (an IR receiver + an IR LED on any two free GPIOs), so this is not cap-gated: it
// works on any build once the hardware is wired. `ir recv <rx_gpio> [seconds]` captures one signal; `ir
// send <tx_gpio>` replays it; `ir show` reports the stored capture. Not an arm-gated offensive op (IR
// light, single-signal replay — a benign utility, not an RF attack).
static int cmd_ir(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "recv") == 0) {
        long rxv = 0, secsv = 0;
        if (!parse_int_arg(argv[2], 0, 48, &rxv) ||
            (argc >= 4 && !parse_int_arg(argv[3], 1, 120, &secsv))) {
            printf("usage: ir recv <rx_gpio 0-48> [seconds 1-120]\n");
            return 0;
        }
        int rx = (int)rxv;
        uint32_t secs = (uint32_t)secsv;
        printf("IR: listening on GPIO %d for up to %us — press a remote button...\n",
               rx, secs ? (unsigned)secs : 8u);
        lxveos_ir_capture_info_t inf;
        esp_err_t e = lxveos_ir_capture(rx, secs * 1000, &inf);
        if (e == ESP_OK) {
            printf("captured %u IR symbols%s — 'ir send <tx_gpio>' to replay\n",
                   (unsigned)inf.symbols, inf.truncated ? " (truncated — signal longer than buffer)" : "");
        } else if (e == ESP_ERR_TIMEOUT) {
            printf("no IR signal received (check the receiver wiring / GPIO)\n");
        } else if (e == ESP_ERR_INVALID_ARG) {
            printf("bad GPIO %d\n", rx);
        } else {
            printf("IR capture failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "send") == 0) {
        long txv = 0;
        if (!parse_int_arg(argv[2], 0, 48, &txv)) {
            printf("usage: ir send <tx_gpio 0-48>\n");
            return 0;
        }
        int tx = (int)txv;
        esp_err_t e = lxveos_ir_replay(tx);
        if (e == ESP_OK) {
            printf("replayed %u IR symbols on GPIO %d\n", (unsigned)lxveos_ir_capture_symbols(), tx);
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("nothing captured yet — 'ir recv <rx_gpio>' first\n");
        } else if (e == ESP_ERR_INVALID_ARG) {
            printf("bad GPIO %d\n", tx);
        } else {
            printf("IR replay failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "show") == 0) {
        if (lxveos_ir_have_capture()) {
            printf("stored IR capture: %u symbols\n", (unsigned)lxveos_ir_capture_symbols());
        } else {
            printf("no IR capture stored\n");
        }
        return 0;
    }
    printf("usage: ir recv <rx_gpio> [seconds] | ir send <tx_gpio> | ir show\n");
    printf("       wire an IR receiver to rx_gpio and an IR LED to tx_gpio (38 kHz). Protocol-agnostic\n");
    printf("       raw capture + replay (universal remote).\n");
    return 0;
}

// subghz — CC1101 sub-GHz radio (subghz_scan recon op, increment 1). `subghz begin <sclk> <miso> <mosi>
// <cs>` brings the SPI link up + identifies the chip; `subghz rssi <mhz>` senses signal strength at a
// frequency; `subghz end` releases the bus. Add-on module on operator-supplied SPI3/VSPI pins; not
// cap-gated. Receive only in this increment (no TX yet, so no arm gate).
// Pull the stored OOK capture, decode it as an EV1527/PT2262 PWM bitstream and print the bits (or why it
// couldn't). Shared by 'subghz capture' (auto-decode on success) and 'subghz decode'. Receive-side only —
// reads the stored capture, transmits nothing.
static void subghz_print_decoded(void)
{
    static uint16_t durs[2 * 256];   // SG_MAX_SYMBOLS symbols -> at most two durations each
    static char bits[257];
    uint32_t nd = lxveos_subghz_capture_durations(durs, sizeof(durs) / sizeof(durs[0]));
    if (nd < 2) {
        printf("  OOK decode: no pulse data captured\n");
        return;
    }
    size_t nb = lxveos_ook_decode(durs, nd, bits, sizeof(bits) - 1);
    if (nb == 0) {
        printf("  OOK decode: no clean PWM frame in %u pulses (raw capture kept for replay)\n",
               (unsigned)nd);
        return;
    }
    bits[nb] = '\0';
    printf("  OOK decode: %u bits  %s\n", (unsigned)nb, bits);
}

static int cmd_subghz(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (argc >= 6 && strcmp(argv[1], "begin") == 0) {
        long sclk = 0, miso = 0, mosi = 0, cs = 0;
        if (!parse_int_arg(argv[2], 0, 48, &sclk) || !parse_int_arg(argv[3], 0, 48, &miso) ||
            !parse_int_arg(argv[4], 0, 48, &mosi) || !parse_int_arg(argv[5], 0, 48, &cs)) {
            printf("usage: subghz begin <sclk> <miso> <mosi> <cs>  (GPIOs 0-48)\n");
            return 0;
        }
        esp_err_t e = lxveos_subghz_begin((int)sclk, (int)miso, (int)mosi, (int)cs);
        if (e == ESP_OK) {
            printf("CC1101 begin: PARTNUM=0x%02X VERSION=0x%02X — %s\n",
                   lxveos_subghz_partnum(), lxveos_subghz_version(),
                   lxveos_subghz_present() ? "module detected" : "NO valid chip (check wiring)");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("subghz already begun — 'subghz end' first\n");
        } else {
            printf("subghz begin failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "rssi") == 0) {
        float mhz = strtof(argv[2], NULL);
        int8_t dbm = 0;
        esp_err_t e = lxveos_subghz_rssi(mhz, &dbm);
        if (e == ESP_OK) {
            printf("RSSI @ %.2f MHz: %d dBm\n", (double)mhz, (int)dbm);
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'subghz begin <sclk> <miso> <mosi> <cs>' first\n");
        } else if (e == ESP_ERR_INVALID_ARG) {
            printf("frequency out of range (300-928 MHz)\n");
        } else {
            printf("subghz rssi failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "capture") == 0) {
        long gdo0v = 0, secsv = 0;
        if (!parse_int_arg(argv[2], 0, 48, &gdo0v) ||
            (argc >= 5 && !parse_int_arg(argv[4], 1, 120, &secsv))) {
            printf("usage: subghz capture <gdo0 0-48> <mhz> [seconds 1-120]\n");
            return 0;
        }
        int gdo0 = (int)gdo0v;
        float mhz = strtof(argv[3], NULL);
        uint32_t secs = (uint32_t)secsv;
        uint32_t n = 0;
        printf("sub-GHz: capturing OOK on GDO0=%d @ %.2f MHz (up to %us)...\n",
               gdo0, (double)mhz, secs ? (unsigned)secs : 8u);
        esp_err_t e = lxveos_subghz_capture(gdo0, mhz, secs * 1000, &n);
        if (e == ESP_OK) {
            printf("captured %u symbols — 'subghz replay <gdo0>' to re-emit (needs arm)\n", (unsigned)n);
            subghz_print_decoded();
        } else if (e == ESP_ERR_TIMEOUT) {
            printf("no signal captured (check GDO0 wiring / frequency)\n");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'subghz begin ...' first\n");
        } else {
            printf("subghz capture failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "replay") == 0) {
        long gdo0v = 0;
        if (!parse_int_arg(argv[2], 0, 48, &gdo0v)) {
            printf("usage: subghz replay <gdo0 0-48>\n");
            return 0;
        }
        int gdo0 = (int)gdo0v;
        esp_err_t e = lxveos_subghz_replay(gdo0);
        if (e == ESP_OK) {
            printf("replayed %u sub-GHz symbols on GDO0=%d (armed)\n",
                   (unsigned)lxveos_subghz_capture_symbols(), gdo0);
        } else if (e == ESP_ERR_NOT_ALLOWED) {
            printf("offensive TX not permitted — run 'arm' first (this is an offensive-TX op).\n");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("nothing captured / not begun — 'subghz begin ...' then 'subghz capture ...' first\n");
        } else {
            printf("subghz replay failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "decode") == 0) {
        if (!lxveos_subghz_have_capture()) {
            printf("nothing captured — 'subghz capture <gdo0> <mhz>' first\n");
            return 0;
        }
        printf("decoding stored OOK capture (%u symbols):\n", (unsigned)lxveos_subghz_capture_symbols());
        subghz_print_decoded();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "end") == 0) {
        lxveos_subghz_end();
        printf("subghz link released\n");
        return 0;
    }
    printf("usage: subghz begin <sclk> <miso> <mosi> <cs> | rssi <mhz> | capture <gdo0> <mhz> [s] | decode |\n");
    printf("       replay <gdo0> | end.  CC1101 on SPI3/VSPI (keep off display SPI2); capture/replay use\n");
    printf("       GDO0 async-serial + RMT. Replay is arm-gated (offensive TX). Single-signal, not a brute.\n");
    return 0;
}

// nRF24 sniff -> mousejack target, remembered between commands.
static uint8_t s_nrf_target[5];
static uint8_t s_nrf_ch;
static bool    s_nrf_have_target;

// nrf24 — nRF24L01+ 2.4 GHz radio. Increment 1: `begin`/`scan` (RPD channel-activity, recv). Increment 2:
// `sniff` (recover a device address) + `mousejack <text>` (arm-gated keystroke injection). Add-on on
// operator-supplied SPI3/VSPI pins + a CE GPIO; not cap-gated.
static int cmd_nrf24(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (argc >= 7 && strcmp(argv[1], "begin") == 0) {
        long sck = 0, miso = 0, mosi = 0, csn = 0, ce = 0;
        if (!parse_int_arg(argv[2], 0, 48, &sck) || !parse_int_arg(argv[3], 0, 48, &miso) ||
            !parse_int_arg(argv[4], 0, 48, &mosi) || !parse_int_arg(argv[5], 0, 48, &csn) ||
            !parse_int_arg(argv[6], 0, 48, &ce)) {
            printf("usage: nrf24 begin <sck> <miso> <mosi> <csn> <ce>  (GPIOs 0-48)\n");
            return 0;
        }
        esp_err_t e = lxveos_nrf24_begin((int)sck, (int)miso, (int)mosi, (int)csn, (int)ce);
        if (e == ESP_OK) {
            printf("nRF24 begin: %s\n", lxveos_nrf24_present() ? "module detected (RF_CH read-back OK)"
                                                               : "NO valid chip (check wiring)");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("nrf24 already begun — 'nrf24 end' first\n");
        } else {
            printf("nrf24 begin failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
        long sweepsv = 0;
        if (argc >= 3 && !parse_int_arg(argv[2], 1, 1000, &sweepsv)) {
            printf("usage: nrf24 scan [sweeps 1-1000]  (default = driver's built-in sweep count)\n");
            return 0;
        }
        uint16_t sweeps = (uint16_t)sweepsv;
        static uint8_t counts[LXVEOS_NRF24_CHANNELS];
        esp_err_t e = lxveos_nrf24_scan(counts, sweeps);
        if (e == ESP_OK) {
            printf("2.4 GHz channel activity (ch: 2400+ch MHz, hits over sweeps):\n");
            for (int ch = 0; ch < LXVEOS_NRF24_CHANNELS; ch++) {
                if (counts[ch] == 0) {
                    continue;   // only list channels that saw energy
                }
                int bars = counts[ch] / 8;
                printf("  ch %3d (%d MHz): %3u ", ch, 2400 + ch, (unsigned)counts[ch]);
                for (int b = 0; b < bars && b < 32; b++) {
                    printf("#");
                }
                printf("\n");
            }
            return 0;
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'nrf24 begin <sck> <miso> <mosi> <csn> <ce>' first\n");
        } else {
            printf("nrf24 scan failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "sniff") == 0) {
        long secsv = 0;
        if (argc >= 3 && !parse_int_arg(argv[2], 1, 120, &secsv)) {
            printf("usage: nrf24 sniff [seconds 1-120]\n");
            return 0;
        }
        uint32_t secs = (uint32_t)secsv;
        printf("nRF24: sniffing for a nearby HID device address (up to %us)...\n", secs ? (unsigned)secs : 8u);
        esp_err_t e = lxveos_nrf24_sniff(s_nrf_target, &s_nrf_ch, secs * 1000);
        if (e == ESP_OK) {
            s_nrf_have_target = true;
            printf("target: %02X:%02X:%02X:%02X:%02X on ch %u — 'nrf24 mousejack <text>' to inject (needs arm)\n",
                   s_nrf_target[0], s_nrf_target[1], s_nrf_target[2], s_nrf_target[3], s_nrf_target[4],
                   s_nrf_ch);
        } else if (e == ESP_ERR_TIMEOUT) {
            printf("no device address recovered (HW-tuning pending — try again / adjust)\n");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'nrf24 begin ...' first\n");
        } else {
            printf("nrf24 sniff failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "mousejack") == 0) {
        if (!s_nrf_have_target) {
            printf("no target — 'nrf24 sniff' first to recover a device address\n");
            return 0;
        }
        char text[128];
        size_t off = 0;
        for (int i = 2; i < argc; i++) {
            int w = snprintf(text + off, sizeof(text) - off, "%s%s", i > 2 ? " " : "", argv[i]);
            if (w < 0 || (size_t)w >= sizeof(text) - off) {
                break;
            }
            off += (size_t)w;
        }
        esp_err_t e = lxveos_nrf24_inject_text(s_nrf_target, s_nrf_ch, text);
        if (e == ESP_OK) {
            printf("mousejack: typed \"%s\" at the target (armed)\n", text);
        } else if (e == ESP_ERR_NOT_ALLOWED) {
            printf("offensive TX not permitted — run 'arm' first (this is an offensive-TX op).\n");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'nrf24 begin ...' first\n");
        } else {
            printf("nrf24 mousejack failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "end") == 0) {
        lxveos_nrf24_end();
        printf("nrf24 link released\n");
        return 0;
    }
    printf("usage: nrf24 begin <sck> <miso> <mosi> <csn> <ce> | scan [sweeps] | sniff [s] |\n");
    printf("       mousejack <text> | end.  Increment 1: RPD channel scan (recv). Increment 2: sniff a\n");
    printf("       device address + arm-gated MouseJack keystroke injection (targeted, not a flood).\n");
    return 0;
}

// nfc — PN532 NFC reader (nfc_read recon op, increment 1). `nfc begin <sda> <scl>` brings up I2C + identifies
// the reader; `nfc read [seconds]` polls for one ISO-14443A card and prints its UID/SAK/ATQA; `nfc end`
// releases. `nfc clone <8hexUID>` writes a spoofed UID to a Gen2 magic card — an offensive-TX op, so it is
// arm-gated (needs `arm` first) exactly like subghz replay / nrf24 mousejack. Add-on I2C pins; not cap-gated.
static int cmd_nfc(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "begin") == 0) {
        long sda = 0, scl = 0;
        if (!parse_int_arg(argv[2], 0, 48, &sda) || !parse_int_arg(argv[3], 0, 48, &scl)) {
            printf("usage: nfc begin <sda> <scl>  (GPIOs 0-48)\n");
            return 0;
        }
        esp_err_t e = lxveos_nfc_begin((int)sda, (int)scl);
        if (e == ESP_OK) {
            printf("PN532 begin: %s (IC=0x%02X ver=0x%02X)\n",
                   lxveos_nfc_present() ? "reader detected" : "NO response (check wiring)",
                   lxveos_nfc_ic(), lxveos_nfc_version());
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("nfc already begun — 'nfc end' first\n");
        } else {
            printf("nfc begin failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "read") == 0) {
        long secsv = 0;
        if (argc >= 3 && !parse_int_arg(argv[2], 1, 120, &secsv)) {
            printf("usage: nfc read [seconds 1-120]\n");
            return 0;
        }
        uint32_t secs = (uint32_t)secsv;
        printf("NFC: present a 13.56 MHz card (up to %us)...\n", secs ? (unsigned)secs : 5u);
        uint8_t uid[10];
        size_t ulen = 0;
        uint8_t sak = 0;
        uint16_t atqa = 0;
        esp_err_t e = lxveos_nfc_read_uid(secs * 1000, uid, sizeof(uid), &ulen, &sak, &atqa);
        if (e == ESP_OK) {
            printf("card UID (%u bytes): ", (unsigned)ulen);
            for (size_t i = 0; i < ulen; i++) {
                printf("%02X", uid[i]);
            }
            printf("  ATQA=0x%04X SAK=0x%02X\n", atqa, sak);
        } else if (e == ESP_ERR_TIMEOUT) {
            printf("no card detected\n");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'nfc begin <sda> <scl>' first\n");
        } else {
            printf("nfc read failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "clone") == 0) {
        const char *h = argv[2];
        uint8_t uid[4];
        if (!parse_hex_octets(h, uid, sizeof(uid))) {  // exactly 8 hex chars -> 4-byte UID (host-tested)
            printf("clone needs a 4-byte UID as 8 hex chars, e.g. 'nfc clone DEADBEEF'\n");
            return 0;
        }
        printf("NFC clone: present a writable/magic Mifare card to write UID %s ...\n", h);
        esp_err_t e = lxveos_nfc_clone_write(uid, sizeof(uid));
        if (e == ESP_OK) {
            printf("wrote UID %s to block 0 (armed)\n", h);
        } else if (e == ESP_ERR_NOT_ALLOWED) {
            printf("offensive TX not permitted — run 'arm' first (this is an offensive-TX op).\n");
        } else if (e == ESP_ERR_TIMEOUT) {
            printf("no card presented\n");
        } else if (e == ESP_ERR_INVALID_STATE) {
            printf("not begun — 'nfc begin <sda> <scl>' first\n");
        } else if (e == ESP_FAIL) {
            printf("auth/write refused — not a Gen2 magic card, or wrong key (HW/card-type pending)\n");
        } else {
            printf("nfc clone failed: %s\n", esp_err_to_name(e));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "end") == 0) {
        lxveos_nfc_end();
        printf("nfc link released\n");
        return 0;
    }
    printf("usage: nfc begin <sda> <scl> | nfc read [seconds] | nfc clone <8hexUID> | nfc end\n");
    printf("       PN532 add-on on I2C. read = identify + read one ISO-14443A UID; clone = write a 4-byte\n");
    printf("       UID to a magic Mifare card's block 0. Emulate is a later increment.\n");
    return 0;
}

// blehid — DEFENSE: passive scan flagging nearby BLE HID devices (keyboards/mice). A rogue BLE keyboard is
// the signature of a keystroke-injection attack (BadBLE / our own `badble`) or a covert BLE keylogger, so
// surfacing them is a defensive recon signal. Reuses the passive BLE scan; a device qualifies if its GAP
// appearance is in the HID category (value >> 6 == 15) or it advertises the HID service UUID 0x1812. Listen
// only. BLE-capability gated.
static int cmd_blehid(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_BLE)) {
        printf("ble capability is not active on this build\n");
        return 0;
    }
    long secsv = 6;
    if (argc >= 2 && !parse_int_arg(argv[1], 1, 60, &secsv)) {
        printf("usage: blehid [seconds 1-60]\n");
        return 0;
    }
    uint32_t secs = (uint32_t)secsv;
    static lxveos_ble_dev_t devs[32];
    size_t found = 0;
    printf("scanning %us for BLE HID devices (keyboards/mice = potential injectors/keyloggers)...\n",
           (unsigned)secs);
    esp_err_t e = lxveos_ble_scan(secs, devs, sizeof(devs) / sizeof(devs[0]), &found);
    if (e != ESP_OK) {
        printf("ble scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    int hits = 0;
    for (size_t i = 0; i < found; i++) {
        bool is_hid = devs[i].appearance_present && (devs[i].appearance >> 6) == 15;
        for (uint8_t u = 0; !is_hid && u < devs[i].svc_uuid_count; u++) {
            if (devs[i].svc_uuids[u] == 0x1812) {   // HID service
                is_hid = true;
            }
        }
        if (!is_hid) {
            continue;
        }
        hits++;
        char appr[24];
        lxveos_ble_appearance_str(devs[i].appearance, appr, sizeof(appr));
        char nm[32];
        sanitize_copy(nm, sizeof(nm), devs[i].name_len ? devs[i].name : "(no name)");
        printf("  [HID] %02X:%02X:%02X:%02X:%02X:%02X  %ddBm  %-10s  %s\n",
               devs[i].addr[5], devs[i].addr[4], devs[i].addr[3],
               devs[i].addr[2], devs[i].addr[1], devs[i].addr[0],
               devs[i].rssi, appr, nm);
        if (lxveos_evt_enabled()) {
            uint8_t a[6] = {devs[i].addr[5], devs[i].addr[4], devs[i].addr[3],
                            devs[i].addr[2], devs[i].addr[1], devs[i].addr[0]};
            char line[160];
            size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
            n = lxveos_evt_kv(line, sizeof(line), n, "kind", "blehid");
            n = lxveos_evt_kv_mac(line, sizeof(line), n, "addr", a);
            n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", devs[i].rssi);
            if (devs[i].name_len) {
                n = lxveos_evt_kv_hex(line, sizeof(line), n, "name",
                                      (const uint8_t *)devs[i].name, devs[i].name_len);
            }
            printf("%s\n", line);
        }
    }
    printf("%d BLE HID device(s) flagged of %u advertisers seen.%s\n", hits, (unsigned)found,
           hits ? " Verify each is an expected keyboard/mouse." : "");
    return 0;
}

// `airspace [ble_seconds]` — CUSTOM: a one-shot airspace occupancy summary (the `airspace_summary` catalog
// op). Runs a passive Wi-Fi AP scan and, when BLE is active, a short passive BLE observe, and reports the
// counts that matter for a quick "what's around me": APs with the open / WPS-exposed / hidden splits, and
// BLE advertisers with the known-tracker count. Emits one `LXVEOS/1 snapshot` event for the CC dashboard.
// Purely analytic over passive scans — transmits nothing. WIFI-gated; BLE counts are best-effort when present.
static int cmd_airspace(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
        printf("wifi capability is not active on this build — cannot summarize airspace\n");
        return 0;
    }
    long bsecs = 4;
    if (argc >= 2 && !parse_int_arg(argv[1], 1, 30, &bsecs)) {
        printf("usage: airspace [ble_seconds 1-30]  (default 4)\n");
        return 0;
    }
    bool ble = lxveos_cap_active(LXVEOS_CAP_BLE);
    printf("airspace summary — passive Wi-Fi scan%s (listen only — no frames sent)...\n",
           ble ? " + BLE observe" : "");
    static lxveos_wifi_ap_t aps[64];
    size_t naps = 0;
    esp_err_t e = lxveos_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &naps);
    if (e != ESP_OK) {
        printf("wifi scan failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    unsigned nopen = 0, nwps = 0, nhidden = 0;
    for (size_t i = 0; i < naps; i++) {
        if (lxveos_wifi_is_open(aps[i].authmode)) {
            nopen++;
        }
        if (aps[i].wps) {
            nwps++;
        }
        if (aps[i].ssid[0] == '\0') {
            nhidden++;
        }
    }
    size_t nble = 0;
    unsigned trackers = 0;
    if (ble) {
        static lxveos_ble_dev_t devs[48];
        esp_err_t be = lxveos_ble_scan((uint32_t)bsecs, devs, sizeof(devs) / sizeof(devs[0]), &nble);
        if (be == ESP_OK) {
            for (size_t i = 0; i < nble; i++) {
                if (devs[i].tracker) {
                    trackers++;
                }
            }
        } else {
            printf("(BLE observe failed: %s — Wi-Fi summary only)\n", esp_err_to_name(be));
            ble = false;
        }
    }
    printf("Wi-Fi: %u AP(s) — %u open, %u WPS-exposed, %u hidden\n", (unsigned)naps, nopen, nwps, nhidden);
    if (ble) {
        printf("BLE:   %u advertiser(s) — %u known tracker(s)\n", (unsigned)nble, trackers);
    } else {
        printf("BLE:   (radio not active — Wi-Fi summary only)\n");
    }
    if (lxveos_evt_enabled()) {
        char line[128];
        size_t n = lxveos_evt_begin(line, sizeof(line), "snapshot");
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "aps", (unsigned long)naps);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "open", nopen);
        n = lxveos_evt_kv_uint(line, sizeof(line), n, "wps", nwps);
        if (ble) {
            n = lxveos_evt_kv_uint(line, sizeof(line), n, "bles", (unsigned long)nble);
            n = lxveos_evt_kv_uint(line, sizeof(line), n, "trackers", trackers);
        }
        printf("%s\n", line);
    }
    return 0;
}

// --- target watchlist (custom X-3 feature) --------------------------------------------------------
// A small persistent list of BSSIDs / BLE addresses to keep an eye out for. `watch scan` runs one
// passive Wi-Fi + BLE sweep and flags any listed target that's on the air right now, emitting one
// `LXVEOS/1 alert kind=watch` per hit for the CC dashboard. Listen-only — the watchlist never transmits.
#define WATCH_MAX   16u  // unsigned: it's compared against size_t counts and printed with %u
#define WATCH_LABEL 24   // NUL-terminated operator note; sanitized on input so it can't garble the console

typedef struct {
    uint8_t mac[6];             // target address, MSB-first (the order `scan` / `blescan` display)
    char    label[WATCH_LABEL]; // optional operator note ("" if none)
} watch_target_t;

static watch_target_t s_watch[WATCH_MAX];
static size_t s_watch_count;

// Index of `mac` in the watchlist, or -1 if absent (exact 6-byte match, addresses stored MSB-first).
static int watch_index_of(const uint8_t mac[6])
{
    for (size_t i = 0; i < s_watch_count; i++) {
        if (memcmp(s_watch[i].mac, mac, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// `watch` — CUSTOM (X-3): a small persistent target watchlist.
//   watch add <mac> [label]   add a target (mac "aa:bb:cc:dd:ee:ff", optional label)
//   watch del <mac>           drop a target
//   watch list                show the watchlist
//   watch clear               empty the watchlist
//   watch scan [ble_seconds]  passive Wi-Fi + BLE sweep; flag + alert any watched target seen now
static int cmd_watch(int argc, char **argv)
{
    if (locked()) {
        return 0;
    }
    const char *sub = argc >= 2 ? argv[1] : "";

    if (strcmp(sub, "list") == 0) {
        if (s_watch_count == 0) {
            printf("watchlist empty — add a target with: watch add <mac> [label]\n");
            return 0;
        }
        printf("watchlist (%u/%u):\n", (unsigned)s_watch_count, WATCH_MAX);
        for (size_t i = 0; i < s_watch_count; i++) {
            const uint8_t *m = s_watch[i].mac;
            printf("  %02x:%02x:%02x:%02x:%02x:%02x  %s\n", m[0], m[1], m[2], m[3], m[4], m[5],
                   s_watch[i].label);
        }
        return 0;
    }

    if (strcmp(sub, "clear") == 0) {
        s_watch_count = 0;
        printf("watchlist cleared\n");
        return 0;
    }

    if (strcmp(sub, "add") == 0) {
        uint8_t mac[6];
        if (argc < 3 || !parse_mac(argv[2], mac)) {
            printf("usage: watch add <mac> [label]  (mac = aa:bb:cc:dd:ee:ff)\n");
            return 0;
        }
        if (watch_index_of(mac) >= 0) {
            printf("already watching %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
                   mac[4], mac[5]);
            return 0;
        }
        if (s_watch_count >= WATCH_MAX) {
            printf("watchlist full (%u max) — remove one first (watch del <mac>)\n", WATCH_MAX);
            return 0;
        }
        watch_target_t *t = &s_watch[s_watch_count];
        memcpy(t->mac, mac, 6);
        if (argc >= 4) {
            sanitize_copy(t->label, sizeof(t->label), argv[3]);  // operator note — keep the console safe
        } else {
            t->label[0] = '\0';
        }
        s_watch_count++;
        printf("watching %02x:%02x:%02x:%02x:%02x:%02x%s%s (%u/%u)\n", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5], t->label[0] ? " " : "", t->label, (unsigned)s_watch_count, WATCH_MAX);
        return 0;
    }

    if (strcmp(sub, "del") == 0) {
        uint8_t mac[6];
        if (argc < 3 || !parse_mac(argv[2], mac)) {
            printf("usage: watch del <mac>\n");
            return 0;
        }
        int idx = watch_index_of(mac);
        if (idx < 0) {
            printf("not on the watchlist: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
                   mac[4], mac[5]);
            return 0;
        }
        for (size_t i = (size_t)idx; i + 1 < s_watch_count; i++) {
            s_watch[i] = s_watch[i + 1];  // compact the array over the removed slot
        }
        s_watch_count--;
        printf("stopped watching %02x:%02x:%02x:%02x:%02x:%02x (%u/%u)\n", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5], (unsigned)s_watch_count, WATCH_MAX);
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        if (!lxveos_cap_active(LXVEOS_CAP_WIFI)) {
            printf("wifi capability is not active on this build — cannot scan\n");
            return 0;
        }
        if (s_watch_count == 0) {
            printf("watchlist empty — add a target first (watch add <mac>)\n");
            return 0;
        }
        long bsecs = 4;
        if (argc >= 3 && !parse_int_arg(argv[2], 1, 30, &bsecs)) {
            printf("usage: watch scan [ble_seconds 1-30]  (default 4)\n");
            return 0;
        }
        bool ble = lxveos_cap_active(LXVEOS_CAP_BLE);
        printf("watch sweep — passive Wi-Fi scan%s (listen only — no frames sent)...\n",
               ble ? " + BLE observe" : "");

        static lxveos_wifi_ap_t aps[64];
        size_t naps = 0;
        esp_err_t e = lxveos_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &naps);
        if (e != ESP_OK) {
            printf("wifi scan failed: %s\n", esp_err_to_name(e));
            return 0;
        }
        static lxveos_ble_dev_t devs[48];
        size_t nble = 0;
        if (ble) {
            esp_err_t be = lxveos_ble_scan((uint32_t)bsecs, devs, sizeof(devs) / sizeof(devs[0]), &nble);
            if (be != ESP_OK) {
                printf("(BLE observe failed: %s — Wi-Fi only)\n", esp_err_to_name(be));
                ble = false;
                nble = 0;
            }
        }

        unsigned hits = 0;
        for (size_t i = 0; i < s_watch_count; i++) {
            const uint8_t *tgt = s_watch[i].mac;
            const char *band = NULL;
            int rssi = 0;
            // Wi-Fi APs store bssid[] MSB-first, matching the operator's typed order — compare directly.
            for (size_t j = 0; j < naps && band == NULL; j++) {
                if (memcmp(aps[j].bssid, tgt, 6) == 0) {
                    band = "wifi";
                    rssi = aps[j].rssi;
                }
            }
            // BLE addr[] is little-endian; reverse to MSB-first before comparing with the target.
            for (size_t j = 0; j < nble && band == NULL; j++) {
                const uint8_t *a = devs[j].addr;
                uint8_t rev[6] = {a[5], a[4], a[3], a[2], a[1], a[0]};
                if (memcmp(rev, tgt, 6) == 0) {
                    band = "ble";
                    rssi = devs[j].rssi;
                }
            }
            if (band != NULL) {
                hits++;
                printf("  PRESENT %02x:%02x:%02x:%02x:%02x:%02x %-4s %ddB  %s\n", tgt[0], tgt[1], tgt[2],
                       tgt[3], tgt[4], tgt[5], band, rssi, s_watch[i].label);
                if (lxveos_evt_enabled()) {
                    char line[128];
                    size_t n = lxveos_evt_begin(line, sizeof(line), "alert");
                    n = lxveos_evt_kv(line, sizeof(line), n, "kind", "watch");
                    n = lxveos_evt_kv_mac(line, sizeof(line), n, "mac", tgt);
                    n = lxveos_evt_kv_int(line, sizeof(line), n, "rssi", rssi);
                    n = lxveos_evt_kv(line, sizeof(line), n, "band", band);
                    printf("%s\n", line);
                }
            }
        }
        printf("watch sweep done — %u/%u target(s) present (%u AP, %u BLE observed)\n", hits,
               (unsigned)s_watch_count, (unsigned)naps, (unsigned)nble);
        return 0;
    }

    printf("usage: watch add <mac> [label] | del <mac> | list | clear | scan [ble_seconds]\n");
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "agree", .help = "Accept the authorized-use terms to unlock commands", .func = &cmd_agree},
        {.command = "info", .help = "Show firmware version, board id, chip and UI profile", .func = &cmd_info},
        {.command = "caps", .help = "List capabilities and whether each is active", .func = &cmd_caps},
        {.command = "features", .help = "List planned/available security operations for this unit", .func = &cmd_features},
        {.command = "scan", .help = "Passive Wi-Fi AP scan (listen only, no frames sent)", .func = &cmd_scan},
        {.command = "sniff", .help = "Passive Wi-Fi packet monitor: sniff [seconds] [channel] (listen only)", .func = &cmd_sniff},
        {.command = "stations", .help = "Passive client-station scan: stations [seconds] [channel] (listen only)", .func = &cmd_stations},
        {.command = "probes", .help = "Passive probe-request SSID logger: probes [seconds] [channel] (listen only)", .func = &cmd_probes},
        {.command = "capture", .help = "Passive EAPOL/PMKID capture -> hashcat 22000: capture [seconds] [channel]", .func = &cmd_capture},
        {.command = "defend", .help = "Passive deauth/disassoc attack detector: defend [seconds] [channel]", .func = &cmd_defend},
        {.command = "pwnwatch", .help = "Passive Pwnagotchi-presence detector: pwnwatch [seconds] [channel] (listen only)", .func = &cmd_pwnwatch},
        {.command = "eviltwin", .help = "Passive evil-twin/rogue-AP detector (duplicate-BSSID ESSIDs)", .func = &cmd_eviltwin},
        {.command = "apaudit", .help = "Passive AP security audit — flag open/WEP/legacy-WPA + WPS-enabled networks (listen only)", .func = &cmd_apaudit},
        {.command = "wardrive", .help = "Passive Wi-Fi wardrive CSV export (bssid,ssid,ch,rssi,auth per line)", .func = &cmd_wardrive},
        {.command = "blewardrive", .help = "Passive BLE wardrive CSV export (addr,type,name,rssi,vendor,tracker per line)", .func = &cmd_blewardrive},
        {.command = "airspace", .help = "Airspace occupancy summary: airspace [ble_seconds] — AP (open/WPS/hidden) + BLE (tracker) counts (listen only)", .func = &cmd_airspace},
        {.command = "watch", .help = "Target watchlist: watch add <mac> [label] | del <mac> | list | clear | scan [ble_seconds] — flag when a watched BSSID/BLE-addr is present (listen only)", .func = &cmd_watch},
        {.command = "blescan", .help = "Passive BLE device scan (+vendor/appearance/service-UUIDs): blescan [seconds] (listen only)", .func = &cmd_blescan},
        {.command = "bleflood", .help = "Passive BLE advert-flood/spam detector: bleflood [seconds] (listen only)", .func = &cmd_bleflood},
        {.command = "btracker", .help = "Passive BLE item-tracker/stalking detector (AirTag/Tile/SmartTag/Chipolo/PebbleBee/GoogleFMN): btracker [seconds]", .func = &cmd_btracker},
        {.command = "flipper", .help = "Passive Flipper Zero detector (BLE service-UUID match): flipper [seconds] (listen only)", .func = &cmd_flipper},
        {.command = "meta", .help = "Passive Meta/Ray-Ban glasses + Oculus detector (BLE mfg/service-ID match): meta [seconds] (listen only)", .func = &cmd_meta},
        {.command = "skimmer", .help = "Passive card-skimmer heuristic — flag default-named HC-0x BT-serial modules: skimmer [seconds] (listen only)", .func = &cmd_skimmer},
        {.command = "flock", .help = "Passive Flock Safety camera heuristic (BLE XUNTONG mfg + name): flock [seconds] — experimental, listen only", .func = &cmd_flock},
        {.command = "surveil", .help = "Counter-surveillance BLE sweep — one scan folds tracker/Flock/Meta/Flipper/skimmer detectors into one summary: surveil [seconds] (listen only)", .func = &cmd_surveil},
        {.command = "sysinfo", .help = "Show ESP-IDF version, reset reason and heap free", .func = &cmd_sysinfo},
        {.command = "status", .help = "One machine-readable status line (Cyber Controller bridge format)", .func = &cmd_status},
        {.command = "bridge", .help = "Toggle machine-readable LXVEOS/1 event emission for the CC bridge: bridge on|off|status", .func = &cmd_bridge},
        {.command = "arm", .help = "Two-factor enable for offensive-TX ops: arm (request), arm <token> (confirm), arm status (query)", .func = &cmd_arm},
        {.command = "disarm", .help = "Hard-disarm: return to SAFE (offensive TX not permitted)", .func = &cmd_disarm},
        {.command = "evilportal", .help = "Rogue AP + captive portal (needs arm): evilportal [ssid|karma|template <id>|templates|creds|stop]", .func = &cmd_evilportal},
        {.command = "badble", .help = "BLE HID keystroke injection (needs arm): badble \"<duckyscript>\" | stop | status", .func = &cmd_badble},
        {.command = "ir", .help = "IR capture + replay (universal remote): ir recv <rx_gpio> [s] | send <tx_gpio> | show", .func = &cmd_ir},
        {.command = "subghz", .help = "Sub-GHz CC1101: begin <sclk> <miso> <mosi> <cs> | rssi <mhz> | capture <gdo0> <mhz> [s] | decode | replay <gdo0> (arm) | end", .func = &cmd_subghz},
        {.command = "nrf24", .help = "nRF24 2.4GHz: begin <sck> <miso> <mosi> <csn> <ce> | scan | sniff | mousejack <text> (arm) | end", .func = &cmd_nrf24},
        {.command = "nfc", .help = "NFC PN532: nfc begin <sda> <scl> | read [seconds] | clone <8hexUID> (arm) | end", .func = &cmd_nfc},
        {.command = "blehid", .help = "DEFENSE: flag nearby BLE HID devices (rogue keyboards/injectors): blehid [seconds]", .func = &cmd_blehid},
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
