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
#include "lxveos_board.h"
#include "lxveos_caps.h"
#include "lxveos_evilportal.h"
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
    size_t ops_ready = 0, ops_planned = 0, ops_unavail = 0;
    lxveos_ops_tally(&ops_ready, &ops_planned, &ops_unavail);
    const char *panel = bsp_display_panel();
    printf("LXVEOS/1 status board=%s chip=%s ui=%s fw=%s panel=%s caps=0x%03x ops=%u/%u/%u heap=%u\n",
           lxveos_board_id(), lxveos_board_chip(), lxveos_ui_profile(), LXVEOS_VERSION,
           (panel && panel[0]) ? panel : "none",
           (unsigned)lxveos_caps_active(),
           (unsigned)ops_ready, (unsigned)ops_planned, (unsigned)ops_unavail,
           (unsigned)esp_get_free_heap_size());
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
    printf("LxveOS operation catalog (M0: drivers land in M1+; attack ops are lab-only)\n");
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
    size_t ready = 0, planned = 0, unavailable = 0;
    lxveos_ops_tally(&ready, &planned, &unavailable);
    printf("summary: %u ready / %u planned / %u unavailable  "
           "(LxveOS authors no jammer/DoS-flood frames)\n",
           (unsigned)ready, (unsigned)planned, (unsigned)unavailable);
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
            printf("  [!] \"%s\": %d BSSIDs (%d open, %d encrypted)%s\n", aps[i].ssid, nbssid,
                   nopen, nenc, (nopen > 0 && nenc > 0) ? "  <- open+encrypted twin" : "");
            for (size_t j = 0; j < found; j++) {
                if (strcmp(aps[j].ssid, aps[i].ssid) == 0) {
                    printf("        %02x:%02x:%02x:%02x:%02x:%02x  ch%-2u %-6s %ddB\n",
                           aps[j].bssid[0], aps[j].bssid[1], aps[j].bssid[2],
                           aps[j].bssid[3], aps[j].bssid[4], aps[j].bssid[5],
                           aps[j].channel, lxveos_wifi_authmode_str(aps[j].authmode), aps[j].rssi);
                }
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
        char ident[48];
        if (d->name_len && appr[0]) {
            snprintf(ident, sizeof(ident), "%s [%s]", d->name, appr);
        } else if (d->name_len) {
            snprintf(ident, sizeof(ident), "%s", d->name);
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
        printf("  %02x:%02x:%02x:%02x:%02x:%02x %-7s %4ddB %-14s %s\n",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0],
               lxveos_ble_addr_type_str(d->addr_type), d->rssi, tn, d->name_len ? d->name : "");
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
        printf("%02x:%02x:%02x:%02x:%02x:%02x,\"", aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
               aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        for (const char *s = aps[i].ssid; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x20) {
                putchar('.');  // sanitize control bytes so they can't break the CSV line
                continue;
            }
            if (c == '"') {
                putchar('"');  // RFC4180: double an embedded quote
            }
            putchar((int)c);
        }
        printf("\",%u,%d,%s,%d\n", aps[i].channel, aps[i].rssi,
               lxveos_wifi_authmode_str(aps[i].authmode), aps[i].ssid[0] ? 0 : 1);
    }
    printf("# %u AP(s) exported (GPS-less — host adds coordinates)\n", (unsigned)found);
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
        return 0;
    }
    uint32_t token = 0;
    esp_err_t e = lxveos_arm_request(&token);
    if (e != ESP_OK) {
        printf("arm request failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("arm requested. Confirm within 30s:  arm %u\n", (unsigned)token);
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
        {.command = "eviltwin", .help = "Passive evil-twin/rogue-AP detector (duplicate-BSSID ESSIDs)", .func = &cmd_eviltwin},
        {.command = "apaudit", .help = "Passive AP security audit — flag open/WEP/legacy-WPA + WPS-enabled networks (listen only)", .func = &cmd_apaudit},
        {.command = "wardrive", .help = "Passive Wi-Fi wardrive CSV export (bssid,ssid,ch,rssi,auth per line)", .func = &cmd_wardrive},
        {.command = "blescan", .help = "Passive BLE device scan (+vendor/appearance/service-UUIDs): blescan [seconds] (listen only)", .func = &cmd_blescan},
        {.command = "bleflood", .help = "Passive BLE advert-flood/spam detector: bleflood [seconds] (listen only)", .func = &cmd_bleflood},
        {.command = "btracker", .help = "Passive BLE item-tracker/stalking detector (AirTag/Tile/SmartTag/Chipolo/PebbleBee/GoogleFMN): btracker [seconds]", .func = &cmd_btracker},
        {.command = "sysinfo", .help = "Show ESP-IDF version, reset reason and heap free", .func = &cmd_sysinfo},
        {.command = "status", .help = "One machine-readable status line (Cyber Controller bridge format)", .func = &cmd_status},
        {.command = "arm", .help = "Two-factor enable for offensive-TX ops: arm (request), then arm <token> (confirm)", .func = &cmd_arm},
        {.command = "disarm", .help = "Hard-disarm: return to SAFE (offensive TX not permitted)", .func = &cmd_disarm},
        {.command = "evilportal", .help = "Rogue AP + captive portal (needs arm): evilportal [ssid|karma|template <id>|templates|creds|stop]", .func = &cmd_evilportal},
        {.command = "badble", .help = "BLE HID keystroke injection (needs arm): badble \"<duckyscript>\" | stop | status", .func = &cmd_badble},
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
