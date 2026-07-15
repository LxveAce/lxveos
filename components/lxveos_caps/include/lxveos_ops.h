#pragma once
// LxveOS operation catalog. The "features from every firmware" vision, encoded as DATA rather than
// prose: one row per security operation LxveOS intends to offer (Wi-Fi scan/monitor/EAPOL-capture,
// BLE scan, PCAP logging, sub-GHz / NFC / IR, and the labelled offensive ops), each tagged with the
// single capability it needs and whether it is implemented yet.
//
// It is deliberately honest: an entry's runtime STATUS is derived from the live capability registry
// (lxveos_cap_active) plus an `implemented` flag, so this never claims a feature the unit can't do.
// A row reads "ready" when implemented AND the board has the capability, "planned" when the capability is
// present but implemented is false, or "unavailable" when the board lacks the capability. VERIFY-NEVER-FAKE:
// `implemented` is true only for a driver that is HW-validated OR runs on a high-confidence built-in-radio
// (Wi-Fi SoftAP) path; every external-radio driver (sub-GHz/nRF24/NFC/IR) and ble_hid_inject stay false until
// real-hardware validation (a green CI build proves compilation, not that the RF works). The catalog is the
// one UI/CC-bridge source of truth; it authors nothing on-air itself.
#include <stdbool.h>
#include <stddef.h>

#include "lxveos_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

// Broad grouping for the operation, so the UI / operator can see intent at a glance. ATTACK ops are the
// labelled offensive features (retained + clearly flagged, never hidden and never silently enabled);
// LxveOS still authors no jammer/DoS-flood/deauth-injection transmit frames — these rows are roadmap metadata only.
typedef enum {
    LXVEOS_OPCAT_RECON = 0,   // passive/active discovery (scans, sniffing, capture)
    LXVEOS_OPCAT_ATTACK,      // offensive (labelled lab-only; no frames authored here)
    LXVEOS_OPCAT_DEFENSE,     // detection / counter-surveillance
    LXVEOS_OPCAT_LOGGING,     // persistence (PCAP, wardrive logs)
    LXVEOS_OPCAT_MISC,        // everything else (IR, etc.)
    LXVEOS_OPCAT_COUNT
} lxveos_opcat_t;

// Runtime availability of an operation on THIS unit, derived from the capability registry.
typedef enum {
    LXVEOS_OP_READY = 0,      // required capability active AND the op is implemented
    LXVEOS_OP_PLANNED,        // required capability active, implementation lands in M1+
    LXVEOS_OP_UNAVAILABLE,    // this board can't do it at all (required capability neither built-in nor add-on)
    LXVEOS_OP_ATTACHABLE      // not present now, but the required cap is an add-on this board can host (wire it)
} lxveos_op_status_t;

// Policy class of an op — how it is handled, distinct from what it does. Lets the CLI, the on-device UI,
// and the Cyber Controller Operate tab render each control correctly (they are NOT uniform across ops).
typedef enum {
    LXVEOS_OPCLASS_STD = 0,     // recon / defense / logging — no offensive-TX gating
    LXVEOS_OPCLASS_OFFENSIVE,   // hub-built offensive TX (portal / PIN / karma / HID / replay / NFC) — arm-gated
    LXVEOS_OPCLASS_RESTRICTED   // jammer / DoS-flood / deauth-injection — hub authors no TX; owner/upstream-supplied
} lxveos_opclass_t;

typedef struct {
    const char *slug;            // stable machine slug for the CC bridge / UI, e.g. "wifi_ap_scan"
    const char *title;           // short human title
    lxveos_opcat_t category;
    lxveos_cap_t required_cap;   // the single capability this op needs to be possible at all
    const char *inspired_by;     // upstream firmware the feature draws from ("Marauder", "Flipper", ...)
    bool implemented;            // true only when HW-validated OR a high-confidence built-in-radio (SoftAP) path
} lxveos_op_t;

// Number of operations in the catalog.
size_t lxveos_ops_count(void);

// The i-th operation, or NULL if out of range. Order is stable (grouped by category then capability).
const lxveos_op_t *lxveos_ops_get(size_t i);

// Runtime status of `op` on this unit. Gates on lxveos_cap_active(), so it reflects the last caps probe.
// A NULL op is reported UNAVAILABLE.
lxveos_op_status_t lxveos_op_status(const lxveos_op_t *op);

// Stable lowercase slug for a category / status ("recon", "planned", ...); "?" if out of range.
const char *lxveos_opcat_name(lxveos_opcat_t c);
const char *lxveos_op_status_name(lxveos_op_status_t s);

// Policy class of `op` (see lxveos_opclass_t). Non-attack ops and NULL are STD; the jammer/DoS-flood
// class is RESTRICTED; every other ATTACK op is OFFENSIVE. Centralises the one policy distinction so the
// CLI / UI / CC bridge never re-derive it.
lxveos_opclass_t lxveos_op_class(const lxveos_op_t *op);
// Stable lowercase slug for a policy class ("std" / "offensive" / "restricted"); "?" if out of range.
const char *lxveos_op_class_name(lxveos_opclass_t k);

// Tally the catalog by runtime status for a one-line summary. Any out-param may be NULL.
void lxveos_ops_tally(size_t *ready, size_t *planned, size_t *attachable, size_t *unavailable);

#ifdef __cplusplus
}
#endif
