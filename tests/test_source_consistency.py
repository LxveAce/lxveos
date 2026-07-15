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
    "subghz_scan", "subghz_replay", "nrf24_scan", "nrf24_mousejack",
    "nfc_read", "nfc_clone", "ir_recv", "ir_send", "ble_hid_inject",
}

# High-confidence built-in-radio / Wi-Fi-SoftAP ops that ARE trusted from a green CI build (implemented=true).
_MUST_BE_IMPLEMENTED = {
    "wifi_ap_scan", "wifi_sta_scan", "wifi_sniff", "wifi_probe_scan", "eapol_capture",
    "deauth_detect", "evil_twin_detect", "wifi_security_audit", "wifi_wardrive",
    "ble_scan", "ble_flood_detect", "ble_tracker_detect", "evil_portal", "karma_ap",
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


def test_cli_numeric_args_use_validated_parsing_not_atoi():
    """CLI numeric args must go through parse_int_arg (which rejects non-numeric input) — never bare
    atoi(argv...), which silently coerces bad input to 0 (a wrong GPIO / a 0-second scan)."""
    calls = re.findall(r"\batoi\s*\(\s*argv", CLI_C)
    assert not calls, f"{len(calls)} bare atoi(argv...) call(s) remain in the CLI — route them through parse_int_arg"


def test_status_bridge_line_exposes_arm_state():
    """The CC bridge status line + its README doc must surface arm state (label, never hide)."""
    assert 'arm=%s' in CLI_C, "cmd_status no longer emits the arm= field"
    assert 'arm=<safe|pending|armed>' in README, "README bridge-line doc missing the arm= field"
