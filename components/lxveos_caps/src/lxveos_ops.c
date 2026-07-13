// LxveOS operation catalog (see lxveos_ops.h). A static, capability-gated table of the security
// operations LxveOS aims to offer — drawn from Marauder and the wider firmware landscape — so the CLI,
// the future on-device menu, and the Cyber Controller bridge all read one honest source of truth for
// "what can this unit do, and what's still to come". Nothing here touches a radio; `implemented` is the
// only thing that flips a row from "planned" to "ready", and it does so only when the real driver lands.
#include "lxveos_ops.h"

#include <stddef.h>

// The catalog. Grouped by category, then by capability. `implemented` is false for every row in M0 —
// no operation has a live driver yet, so each reads "planned" (capability present) or "unavailable"
// (capability absent) at runtime. Keep new rows in category order; the CC bridge keys on `slug`.
static const lxveos_op_t OPS[] = {
    // ── Recon ────────────────────────────────────────────────────────────────────────────────────
    {"wifi_ap_scan",   "Wi-Fi AP scan",         LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", true},
    {"wifi_sta_scan",  "Wi-Fi station scan",    LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", false},
    {"wifi_sniff",     "Wi-Fi packet monitor",  LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", false},
    {"eapol_capture",  "EAPOL/PMKID capture",   LXVEOS_OPCAT_RECON,   LXVEOS_CAP_WIFI,    "Marauder", false},
    {"ble_scan",       "BLE device scan",       LXVEOS_OPCAT_RECON,   LXVEOS_CAP_BLE,     "Marauder", false},
    {"subghz_scan",    "Sub-GHz scan",          LXVEOS_OPCAT_RECON,   LXVEOS_CAP_SUBGHZ,  "Flipper",  false},
    {"nfc_read",       "NFC tag read",          LXVEOS_OPCAT_RECON,   LXVEOS_CAP_NFC,     "Flipper",  false},
    // ── Attack (labelled lab-only; no frames authored here) ───────────────────────────────────────
    {"beacon_flood",   "Beacon flood",          LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_WIFI,    "Marauder", false},
    {"deauth_burst",   "Deauth burst",          LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_WIFI,    "Marauder", false},
    {"ble_spam",       "BLE advertise spam",    LXVEOS_OPCAT_ATTACK,  LXVEOS_CAP_BLE,     "multi",    false},
    // ── Defense ───────────────────────────────────────────────────────────────────────────────────
    {"deauth_detect",  "Deauth/pwn detector",   LXVEOS_OPCAT_DEFENSE, LXVEOS_CAP_WIFI,    "Marauder", false},
    // ── Logging ───────────────────────────────────────────────────────────────────────────────────
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
    if (op == NULL || !lxveos_cap_active(op->required_cap)) {
        return LXVEOS_OP_UNAVAILABLE;
    }
    return op->implemented ? LXVEOS_OP_READY : LXVEOS_OP_PLANNED;
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
    if ((int)s < 0 || s > LXVEOS_OP_UNAVAILABLE) {
        return "?";
    }
    return STATUS_NAMES[s];
}

void lxveos_ops_tally(size_t *ready, size_t *planned, size_t *unavailable)
{
    size_t r = 0, p = 0, u = 0;
    for (size_t i = 0; i < OPS_N; i++) {
        switch (lxveos_op_status(&OPS[i])) {
        case LXVEOS_OP_READY:       r++; break;
        case LXVEOS_OP_PLANNED:     p++; break;
        case LXVEOS_OP_UNAVAILABLE: u++; break;
        }
    }
    if (ready) {
        *ready = r;
    }
    if (planned) {
        *planned = p;
    }
    if (unavailable) {
        *unavailable = u;
    }
}
