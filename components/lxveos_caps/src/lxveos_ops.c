// LxveOS operation catalog (see lxveos_ops.h). A static, capability-gated table of the security
// operations LxveOS aims to offer — drawn from Marauder and the wider firmware landscape — so the CLI,
// the future on-device menu, and the Cyber Controller bridge all read one honest source of truth for
// "what can this unit do, and what's still to come". Nothing here touches a radio; `implemented` is the
// only thing that flips a row from "planned" to "ready", and it does so only when the real driver lands.
#include "lxveos_ops.h"

#include <stddef.h>
#include <string.h>

// The catalog. Grouped by category, then by capability. VERIFY-NEVER-FAKE rule for `implemented`: set it true
// ONLY when a real driver exists AND that driver is either HW-validated OR runs on a high-confidence built-in
// radio path (Wi-Fi SoftAP), where a green CI build is enough to trust. Every EXTERNAL-radio driver
// (sub-GHz / nRF24 / NFC / IR) and ble_hid_inject stay `implemented=false` until validated on real hardware —
// CI proves the code compiles, never that the RF actually works. A row reads "ready" when implemented AND the
// board has the capability, "planned" when the capability is present but implemented is false, "unavailable"
// when the board lacks the capability. Keep new rows in category order; the CC bridge keys on `slug`.
static const lxveos_op_t OPS[] = {
    // ── Recon ────────────────────────────────────────────────────────────────────────────────────
    {"wifi_ap_scan",   "Wi-Fi AP scan",         LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", true},
    {"wifi_sta_scan",  "Wi-Fi station scan",    LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", true},
    {"wifi_sniff",     "Wi-Fi packet monitor",  LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", true},
    {"wifi_probe_scan", "Probe-request SSID log", LXVEOS_OPCAT_RECON,  LXVEOS_CAP_WIFI,    "Marauder", true},
    {"wifi_5ghz_scan", "Wi-Fi 5 GHz AP scan",   LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "custom",   false},
    {"eapol_capture",  "EAPOL/PMKID capture",   LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", true},
    {"airspace_summary", "Airspace occupancy summary", LXVEOS_OPCAT_RECON, LXVEOS_CAP_WIFI, "custom",  true},
    {"ble_scan",       "BLE device scan",       LXVEOS_OPCAT_RECON,   LXVEOS_CAP_BLE,     "Marauder", true},
    {"subghz_scan",    "Sub-GHz scan",          LXVEOS_OPCAT_RECON,   LXVEOS_CAP_SUBGHZ,  "Flipper",  false},
    {"nfc_read",       "NFC tag read",          LXVEOS_OPCAT_RECON,   LXVEOS_CAP_NFC,     "Flipper",  false},
    {"ir_recv",        "IR receive/decode",     LXVEOS_OPCAT_RECON,   LXVEOS_CAP_IR,      "Flipper",  false},
    // ── Attack ────────────────────────────────────────────────────────────────────────────────────
    // Hub-built offensive: protocol-level attacks (NOT jammers/floods), authorized-lab. Each row flips
    // to "ready" as its driver lands; the TX these author is protocol traffic (portal/PIN/replay/HID/
    // NFC), never a jammer or a mass-DoS flood.
    {"evil_portal",    "Evil-portal captive portal", LXVEOS_OPCAT_ATTACK, LXVEOS_CAP_WIFI, "Marauder", true},
    {"wps_attack",     "WPS-PIN attack",        LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_WIFI,    "Bruce",    false},
    {"karma_ap",       "Karma / probe-response AP", LXVEOS_OPCAT_ATTACK, LXVEOS_CAP_WIFI, "Pineapple", true},
    {"ble_hid_inject", "BLE HID injection",     LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_BLE,     "Bruce",    false},
    {"subghz_replay",  "Sub-GHz capture+replay", LXVEOS_OPCAT_ATTACK, LXVEOS_CAP_SUBGHZ,  "Flipper",  false},
    {"subghz_brute",   "Sub-GHz de Bruijn brute", LXVEOS_OPCAT_ATTACK, LXVEOS_CAP_SUBGHZ, "Flipper",  false},
    {"nrf24_mousejack", "nRF24 Mousejack inject", LXVEOS_OPCAT_ATTACK, LXVEOS_CAP_NRF24,  "Bastille", false},
    {"nfc_clone",      "NFC clone",             LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_NFC,     "Flipper",  false},
    {"nfc_emulate",    "NFC emulate",           LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_NFC,     "Flipper",  false},
    // Jammer / DoS-flood / deauth-injection class: catalogued + control-surfaced ONLY. The hub does not
    // author these TX payloads (pure denial-of-service); the emitter is owner/upstream-supplied.
    {"beacon_flood",   "Beacon flood",          LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_WIFI,    "Marauder", false},
    {"deauth_burst",   "Deauth/disassoc burst", LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_WIFI,    "Marauder", false},
    {"handshake_force", "Active handshake force (deauth)", LXVEOS_OPCAT_ATTACK, LXVEOS_CAP_WIFI, "Marauder", false},
    {"ble_spam",       "BLE advertise spam",    LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_BLE,     "multi",    false},
    // ── Defense ───────────────────────────────────────────────────────────────────────────────────
    {"deauth_detect",  "Deauth/pwn detector",   LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_WIFI,    "Marauder", true},
    {"evil_twin_detect", "Evil-twin/rogue-AP detector", LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_WIFI, "custom", true},
    {"wifi_security_audit", "AP security-posture audit", LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_WIFI, "custom", true},
    {"ble_flood_detect", "BLE advert-flood detector", LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_BLE, "custom", true},
    {"ble_tracker_detect", "BLE item-tracker/stalking detector", LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_BLE, "custom", true},
    {"ble_hid_detect", "Rogue BLE-HID detector", LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_BLE, "custom", true},
    {"target_watch",   "Target-address watchlist", LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_WIFI, "custom", true},
    // ── Logging ───────────────────────────────────────────────────────────────────────────────────
    {"wifi_wardrive",  "Wi-Fi wardrive CSV",    LXVEOS_OPCAT_LOGGING, LXVEOS_CAP_WIFI,    "Marauder", true},
    {"pcap_log",       "PCAP capture to SD",    LXVEOS_OPCAT_LOGGING, LXVEOS_CAP_STORAGE, "Marauder", false},
    {"wardrive_log",   "GPS wardrive log",      LXVEOS_OPCAT_LOGGING, LXVEOS_CAP_GPS,     "Marauder", false},
    // ── Misc ──────────────────────────────────────────────────────────────────────────────────────
    {"ir_send",        "IR transmit",           LXVEOS_OPCAT_MISC,    LXVEOS_CAP_IR,      "Flipper",  false},
    {"nrf24_scan",     "nRF24 channel scan",    LXVEOS_OPCAT_MISC,    LXVEOS_CAP_NRF24,   "multi",    false},
};

#define OPS_N (sizeof(OPS) / sizeof(OPS[0]))

static const char *const OPCAT_NAMES[LXVEOS_OPCAT_COUNT] = {
    [LXVEOS_OPCAT_RECON]   = "recon",
    [LXVEOS_OPCAT_ATTACK]  = "attack",
    [LXVEOS_OPCAT_DEFENSE] = "defense",
    [LXVEOS_OPCAT_LOGGING] = "logging",
    [LXVEOS_OPCAT_MISC]    = "misc",
};

static const char *const STATUS_NAMES[] = {
    [LXVEOS_OP_READY]       = "ready",
    [LXVEOS_OP_PLANNED]     = "planned",
    [LXVEOS_OP_UNAVAILABLE] = "unavailable",
    [LXVEOS_OP_ATTACHABLE]  = "attachable",
};

size_t lxveos_ops_count(void)
{
    return OPS_N;
}

const lxveos_op_t *lxveos_ops_get(size_t i)
{
    return i < OPS_N ? &OPS[i] : NULL;
}

lxveos_op_status_t lxveos_op_status(const lxveos_op_t *op)
{
    if (op == NULL) {
        return LXVEOS_OP_UNAVAILABLE;
    }
    if (lxveos_cap_active(op->required_cap)) {
        return op->implemented ? LXVEOS_OP_READY : LXVEOS_OP_PLANNED;
    }
    // Cap isn't active. If it's an attachable add-on for this board, say so (wire the module) rather than
    // reporting a flat "unavailable" — an honest middle state, still not usable until the module is present.
    if (lxveos_cap_is_addon(op->required_cap)) {
        return LXVEOS_OP_ATTACHABLE;
    }
    return LXVEOS_OP_UNAVAILABLE;
}

const char *lxveos_opcat_name(lxveos_opcat_t c)
{
    if ((int)c < 0 || c >= LXVEOS_OPCAT_COUNT || OPCAT_NAMES[c] == NULL) {
        return "?";
    }
    return OPCAT_NAMES[c];
}

const char *lxveos_op_status_name(lxveos_op_status_t s)
{
    if ((int)s < 0 || s > LXVEOS_OP_ATTACHABLE) {
        return "?";
    }
    return STATUS_NAMES[s];
}

static const char *const OPCLASS_NAMES[] = {
    [LXVEOS_OPCLASS_STD]        = "std",
    [LXVEOS_OPCLASS_OFFENSIVE]  = "offensive",
    [LXVEOS_OPCLASS_RESTRICTED] = "restricted",
};

// The jammer / DoS-flood / deauth-injection slugs: catalogued + control-surfaced, but the hub authors no
// TX payload for them (pure denial-of-service) — the emitter is owner/upstream-supplied. Keep in sync with
// the two labelled Attack groups in OPS[] above.
static const char *const RESTRICTED_SLUGS[] = {
    "beacon_flood", "deauth_burst", "handshake_force", "ble_spam",
};

lxveos_opclass_t lxveos_op_class(const lxveos_op_t *op)
{
    if (op == NULL || op->category != LXVEOS_OPCAT_ATTACK) {
        return LXVEOS_OPCLASS_STD;
    }
    for (size_t i = 0; i < sizeof(RESTRICTED_SLUGS) / sizeof(RESTRICTED_SLUGS[0]); i++) {
        if (strcmp(op->slug, RESTRICTED_SLUGS[i]) == 0) {
            return LXVEOS_OPCLASS_RESTRICTED;
        }
    }
    return LXVEOS_OPCLASS_OFFENSIVE;
}

const char *lxveos_op_class_name(lxveos_opclass_t k)
{
    if ((int)k < 0 || k > LXVEOS_OPCLASS_RESTRICTED) {
        return "?";
    }
    return OPCLASS_NAMES[k];
}

void lxveos_ops_tally(size_t *ready, size_t *planned, size_t *attachable, size_t *unavailable)
{
    size_t r = 0, p = 0, a = 0, u = 0;
    for (size_t i = 0; i < OPS_N; i++) {
        switch (lxveos_op_status(&OPS[i])) {
        case LXVEOS_OP_READY:       r++; break;
        case LXVEOS_OP_PLANNED:     p++; break;
        case LXVEOS_OP_ATTACHABLE:  a++; break;
        case LXVEOS_OP_UNAVAILABLE: u++; break;
        }
    }
    if (ready) {
        *ready = r;
    }
    if (planned) {
        *planned = p;
    }
    if (attachable) {
        *attachable = a;
    }
    if (unavailable) {
        *unavailable = u;
    }
}
