"""Host-side (no-ESP-IDF) source-consistency guards for LxveOS.

These are cheap regex/text checks over the C sources that catch the classes of drift a firmware heading
toward publishment must not ship:

  * an offensive-TX op whose driver forgets the two-factor `arm` gate (a real safety-contract bug found in
    the nfc_clone path — this test would have caught it),
  * a registered CLI command that is missing from the README command table (the docs silently falling behind
    the REPL), and
  * a shipped, CLI-reachable operation still flagged `implemented=false` in the op catalog (so `features` /
    the CC `status` bridge under-report what the unit can do).

They run with plain `python -m pytest tests/` (no toolchain) alongside the board-config pipeline tests.
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI_C = (ROOT / "components/lxveos_cli/src/lxveos_cli.c").read_text(encoding="utf-8")
OPS_C = (ROOT / "components/lxveos_caps/src/lxveos_ops.c").read_text(encoding="utf-8")
README = (ROOT / "README.md").read_text(encoding="utf-8")

# Commands that esp_console provides itself / that are not operator commands to document.
_BUILTIN_CMDS = {"help"}


def _registered_commands() -> set[str]:
    """Every command registered in the esp_console table: `.command = "X"`."""
    return set(re.findall(r'\.command\s*=\s*"([^"]+)"', CLI_C)) - _BUILTIN_CMDS


def _catalog() -> dict[str, bool]:
    """slug -> implemented, parsed from the OPS[] table in lxveos_ops.c."""
    rows = re.findall(
        r'\{"(\w+)",\s*"[^"]*",\s*\w+,\s*\w+,\s*"[^"]*",\s*(true|false)\}', OPS_C
    )
    assert rows, "could not parse the OPS[] catalog table — did lxveos_ops.c format change?"
    return {slug: flag == "true" for slug, flag in rows}


def test_every_cli_command_is_documented_in_readme():
    """Each registered CLI command must appear (as a backtick-quoted token) in the README."""
    missing = []
    for cmd in sorted(_registered_commands()):
        # Match the command as the leading token inside a backtick span, e.g. `ir recv ...` / `arm` /
        # `nfc begin ...` — a word boundary after the name avoids matching substrings like "their".
        if not re.search(r"`" + re.escape(cmd) + r"[ `|]", README):
            missing.append(cmd)
    assert not missing, f"CLI commands missing from the README command table: {missing}"


# Every offensive-TX op's driver MUST consult the arm gate before it can emit. Maps the op to the source
# file that performs the transmit. (Regression guard for the nfc_clone-bypassed-the-gate finding.)
_OFFENSIVE_TX_DRIVERS = {
    "nfc clone": "components/lxveos_nfc/src/lxveos_nfc.c",
    "subghz replay": "components/lxveos_subghz/src/lxveos_subghz.c",
    "nrf24 mousejack": "components/lxveos_nrf24/src/lxveos_nrf24.c",
    "badble (BLE HID inject)": "components/lxveos_ble/src/lxveos_ble_hid.c",
    "evil-portal / karma": "components/lxveos_evilportal/src/lxveos_evilportal.c",
}


def test_offensive_tx_drivers_check_the_arm_gate():
    ungated = []
    for op, rel in _OFFENSIVE_TX_DRIVERS.items():
        src = (ROOT / rel).read_text(encoding="utf-8")
        if "lxveos_arm_can_emit" not in src:
            ungated.append(f"{op} ({rel})")
    assert not ungated, "offensive-TX op(s) not guarded by lxveos_arm_can_emit(): " + ", ".join(ungated)


# VERIFY-NEVER-FAKE (LXVEOS-BUILD-HANDOFF section 2): the external-radio drivers (sub-GHz / nRF24 / NFC / IR)
# and ble_hid_inject stay implemented=false until validated on real hardware — a green CI build proves the code
# compiles, never that the RF/HID actually works. This guard fails if any is flipped true off green CI alone.
_MUST_STAY_UNVALIDATED = {
    "subghz_scan", "subghz_replay", "subghz_brute", "nrf24_scan", "nrf24_mousejack",
    "nfc_read", "nfc_clone", "nfc_emulate", "ir_recv", "ir_send", "ble_hid_inject",
}

# High-confidence built-in-radio / Wi-Fi-SoftAP ops that ARE trusted from a green CI build (implemented=true).
_MUST_BE_IMPLEMENTED = {
    "wifi_ap_scan", "wifi_sta_scan", "wifi_sniff", "wifi_probe_scan", "eapol_capture",
    "deauth_detect", "pwnagotchi_detect", "evil_twin_detect", "wifi_security_audit", "wifi_wardrive",
    "ble_scan", "ble_wardrive", "ble_flood_detect", "ble_tracker_detect", "flipper_detect",
    "meta_detect", "skimmer_detect", "flock_detect", "surveil_scan", "ble_hid_detect",
    "evil_portal", "karma_ap", "airspace_summary", "target_watch",
}

# The interference-emitter class (RF jammer / deauth-flood / beacon-flood / BLE advert-spam) is a FIXED
# boundary (SAFEGUARDS-LOG jammer-tx): control-surface catalog rows only, never an authored TX loop.
_INTERFERENCE_EMITTERS = {"beacon_flood", "deauth_burst", "handshake_force", "ble_spam"}


def test_external_radio_and_hid_ops_stay_unvalidated():
    cat = _catalog()
    unknown = _MUST_STAY_UNVALIDATED - set(cat)
    assert not unknown, f"expected op slugs not found in the catalog: {sorted(unknown)}"
    premature = sorted(s for s in _MUST_STAY_UNVALIDATED if cat[s])
    assert not premature, (
        "external-radio / HID op(s) flagged implemented=true without HW validation "
        f"(verify-never-fake, LXVEOS-BUILD-HANDOFF section 2): {premature}"
    )


def test_high_confidence_ops_are_implemented():
    cat = _catalog()
    unknown = _MUST_BE_IMPLEMENTED - set(cat)
    assert not unknown, f"expected op slugs not found in the catalog: {sorted(unknown)}"
    missing = sorted(s for s in _MUST_BE_IMPLEMENTED if not cat[s])
    assert not missing, f"built-in/SoftAP op(s) that should be implemented=true but are false: {missing}"


def test_interference_emitters_are_never_implemented():
    cat = _catalog()
    live = sorted(s for s in _INTERFERENCE_EMITTERS if cat.get(s))
    assert not live, (
        "interference emitter flagged implemented=true — boundary-excluded, no authored TX loops "
        f"(SAFEGUARDS-LOG jammer-tx): {live}"
    )


def test_every_implemented_op_is_in_exactly_one_guard_set():
    """Completeness: every op flagged implemented=true in the catalog must be classified by EXACTLY ONE honesty
    guard set (must-be-implemented / must-stay-unvalidated / interference-emitters). This is the invariant that
    would have caught airspace_summary + target_watch sitting outside every set — a new implemented=true op added
    without a guard now fails loudly instead of silently escaping the verify-never-fake checks."""
    cat = _catalog()
    guards = {
        "_MUST_BE_IMPLEMENTED": _MUST_BE_IMPLEMENTED,
        "_MUST_STAY_UNVALIDATED": _MUST_STAY_UNVALIDATED,
        "_INTERFERENCE_EMITTERS": _INTERFERENCE_EMITTERS,
    }
    unclassified, multi = [], []
    for slug, implemented in cat.items():
        if not implemented:
            continue
        members = [name for name, members_set in guards.items() if slug in members_set]
        if not members:
            unclassified.append(slug)
        elif len(members) > 1:
            multi.append(f"{slug} in {members}")
    assert not unclassified, (
        f"implemented=true op(s) outside every honesty-guard set (add to the right set in this file): "
        f"{sorted(unclassified)}"
    )
    assert not multi, f"implemented=true op(s) classified into more than one guard set: {multi}"


def test_cli_numeric_args_use_validated_parsing_not_atoi():
    """CLI numeric args must go through parse_int_arg (which rejects non-numeric input) — never bare
    atoi(argv...), which silently coerces bad input to 0 (a wrong GPIO / a 0-second scan)."""
    calls = re.findall(r"\batoi\s*\(\s*argv", CLI_C)
    assert not calls, f"{len(calls)} bare atoi(argv...) call(s) remain in the CLI — route them through parse_int_arg"


def test_device_supplied_names_are_console_sanitized():
    """A device-supplied Wi-Fi SSID / BLE local name must never reach a raw printf %s — a crafted name can
    carry terminal escapes (cursor moves / screen clears) that garble or spoof the operator's console. The
    plain-print sites (eviltwin / blescan / btracker / blehid) route the name through sanitize_copy()
    (control bytes -> '.'); probes / apaudit / wardrive sanitize inline where they also pad or CSV-quote.
    This guard fails if one of the hardened sites regresses to printing the raw device string."""
    # sanitize_copy() lives in the lxveos_cliutil component (host-tested); the CLI includes it and calls it
    # at each plain-print device-name site.
    assert '#include "lxveos_cliutil.h"' in CLI_C, "the CLI no longer includes the console sanitizer"
    assert CLI_C.count("sanitize_copy(") >= 4, "a device-name print site lost its sanitize_copy() call"
    # the specific raw-name-into-printf patterns that were hardened must stay gone
    raw_patterns = {
        "blescan ident": '"%s [%s]", d->name',
        "eviltwin flagged": ", aps[i].ssid, nbssid",
        "btracker row": "d->rssi, tn, d->name_len ? d->name",
        "blehid row": "devs[i].rssi, appr, devs[i].name_len ? devs[i].name",
    }
    regressed = sorted(site for site, pat in raw_patterns.items() if pat in CLI_C)
    assert not regressed, f"raw device-name print path(s) reintroduced (route through sanitize_copy): {regressed}"


def test_status_bridge_line_exposes_arm_state():
    """The CC bridge status line + its README doc must surface arm state + tx-compiled (label, never hide)."""
    assert 'arm=%s' in CLI_C, "cmd_status no longer emits the arm= field"
    assert 'arm=<safe|pending|armed>' in README, "README bridge-line doc missing the arm= field"
    assert 'tx=%d' in CLI_C, "cmd_status no longer emits the tx= (offensive-TX-compiled) field"
    assert 'tx=<0|1>' in README, "README bridge-line doc missing the tx= field"


# ── CLI command <-> op-catalog coverage ────────────────────────────────────────────────────────────
# The catalog (lxveos_ops.c OPS[]) is the SSOT the CC bridge reads, so every security CLI verb must map
# to a real catalog slug, and every catalog slug must either have a CLI verb or be a KNOWN cli-less row.
# This is the guard that would have caught the `blehid` drift (a shipped detector with no catalog row, so
# `features`/`status` under-reported the unit). Curated because one verb can drive several slugs
# (subghz/nfc/nrf24/ir subcommands) and several slugs are intentionally cli-less.

# Introspection/utility verbs that intentionally have NO catalog op.
_UTILITY_CMDS = {
    "agree", "arm", "disarm", "caps", "features", "info", "status", "sysinfo",
    "loglevel", "nvs", "reboot", "bridge",
}

# Each security verb -> the catalog slug(s) it drives (subcommands fold into the parent verb).
_CLI_TO_SLUGS = {
    "scan": {"wifi_ap_scan"},
    "sniff": {"wifi_sniff"},
    "stations": {"wifi_sta_scan"},
    "probes": {"wifi_probe_scan"},
    "capture": {"eapol_capture"},
    "airspace": {"airspace_summary"},
    "watch": {"target_watch"},
    "wardrive": {"wifi_wardrive"},
    "blewardrive": {"ble_wardrive"},
    "blescan": {"ble_scan"},
    "defend": {"deauth_detect"},
    "pwnwatch": {"pwnagotchi_detect"},
    "eviltwin": {"evil_twin_detect"},
    "apaudit": {"wifi_security_audit"},
    "bleflood": {"ble_flood_detect"},
    "btracker": {"ble_tracker_detect"},
    "flipper": {"flipper_detect"},
    "meta": {"meta_detect"},
    "skimmer": {"skimmer_detect"},
    "flock": {"flock_detect"},
    "surveil": {"surveil_scan"},
    "blehid": {"ble_hid_detect"},
    "evilportal": {"evil_portal", "karma_ap"},
    "badble": {"ble_hid_inject"},
    "subghz": {"subghz_scan", "subghz_replay", "subghz_brute"},
    "nrf24": {"nrf24_scan", "nrf24_mousejack"},
    "nfc": {"nfc_read", "nfc_clone", "nfc_emulate"},
    "ir": {"ir_recv", "ir_send"},
}

# Catalog slugs with NO CLI verb by design:
#  - interference emitters: control-surface rows only, never an authored TX loop (fixed boundary)
#  - idf-infeasible: no honest driver on the ESP32/S3 stack (5 GHz PHY / WPS external-registrar)
#  - fleet-board roadmap: needs SD (pcap_log) or a GPS module (wardrive_log)
_SLUGS_WITHOUT_CLI = (
    _INTERFERENCE_EMITTERS
    | {"wps_attack", "wifi_5ghz_scan"}
    | {"pcap_log", "wardrive_log"}
)


def test_every_security_cli_command_maps_to_a_catalog_slug():
    """Every registered non-utility CLI verb must map to catalog slug(s) that exist — so a new security
    command can't ship without a catalog row (the `blehid` drift this guard was added for)."""
    cat = _catalog()
    security = _registered_commands() - _UTILITY_CMDS
    unmapped = sorted(security - set(_CLI_TO_SLUGS))
    assert not unmapped, (
        f"security CLI verb(s) with no catalog mapping: {unmapped} — add them to _CLI_TO_SLUGS "
        "(and give each a catalog row in lxveos_ops.c)"
    )
    for cmd in sorted(_CLI_TO_SLUGS):
        absent = sorted(s for s in _CLI_TO_SLUGS[cmd] if s not in cat)
        assert not absent, f"CLI verb `{cmd}` maps to slug(s) absent from the catalog: {absent}"


def test_every_catalog_slug_has_a_cli_or_is_known_cli_less():
    """Every catalog slug must either be driven by a CLI verb or be an explicitly cli-less row — so a slug
    can't silently lose its command (or be added unreachable) without being classified."""
    cat = _catalog()
    reachable: set[str] = set().union(*_CLI_TO_SLUGS.values())
    orphan = sorted(s for s in cat if s not in reachable and s not in _SLUGS_WITHOUT_CLI)
    assert not orphan, (
        f"catalog slug(s) with no CLI verb and not classified cli-less: {orphan} — wire a command "
        "or add to _SLUGS_WITHOUT_CLI (emitter-excluded / idf-infeasible / fleet-roadmap)"
    )
