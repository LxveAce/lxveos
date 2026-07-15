"""Host-side (no-ESP-IDF) tests for the board-config pipeline: cyd_boards.json -> gen_board_configs.py.

The manifest is the SSOT shared with Cyber Controller, and the per-board build inputs are generated from
it. A manifest typo (unknown flash size, missing partition CSV, target/chip mismatch, incomplete display
block) would otherwise only surface deep in the ESP-IDF build — or silently produce a wrong image. These
run in CI before the build matrix as a fast gate, and locally with `python -m pytest`.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import gen_board_configs as g  # noqa: E402

BOARDS = json.loads((ROOT / "cyd_boards.json").read_text(encoding="utf-8"))["boards"]

# The M0 Tier-1 fleet the README + CI matrix commit to.
M0_TIER1 = {"bare_esp32_headless", "cyd_2432S028_classic", "m5stickc_plus2",
            "m5cardputer_v1", "jc3248w535_s3_qspi"}


def test_manifest_is_valid():
    errs = g.validate_manifest(BOARDS)
    assert errs == [], "cyd_boards.json validation failed:\n" + "\n".join(errs)


def test_m0_tier1_boards_present():
    assert M0_TIER1 <= set(BOARDS), f"missing M0 boards: {M0_TIER1 - set(BOARDS)}"


def test_referenced_partition_csvs_exist():
    for bid, b in BOARDS.items():
        csv = b.get("build", {}).get("partition_csv")
        if csv:
            assert (ROOT / csv).is_file(), f"{bid}: partition CSV {csv} missing"


def test_generated_sdkconfig_is_wellformed():
    for bid, b in BOARDS.items():
        text = g.sdkconfig_lines(bid, b)
        assert f'CONFIG_IDF_TARGET="{b.get("build", {}).get("idf_target", b.get("chip"))}"' in text
        # exactly one flash-size config, exactly one optimization config
        assert sum(text.count(v) for v in g.FLASH.values()) == 1, f"{bid}: not exactly one flash size"
        assert ("CONFIG_COMPILER_OPTIMIZATION_PERF=y" in text) ^ \
               ("CONFIG_COMPILER_OPTIMIZATION_SIZE=y" in text), f"{bid}: optimization line"


def test_addon_features_emit_addon_config():
    """A feature marked "addon" (external module on operator pins) emits CONFIG_LXVEOS_ADDON_<CAP>=y and NOT
    HAS_<CAP>, so the op catalog reports it "attachable" rather than a flat "unavailable"; a True feature
    emits HAS_ and never ADDON_ (a cap is never both). Guards the generator against silently dropping the
    addon rows again."""
    saw_addon = False
    for bid, b in BOARDS.items():
        text = g.sdkconfig_lines(bid, b)
        feats = b.get("features", {})
        for k in g.ADDON_KEYS:
            has = f"CONFIG_LXVEOS_HAS_{k.upper()}=y"
            addon = f"CONFIG_LXVEOS_ADDON_{k.upper()}=y"
            v = feats.get(k)
            if v == "addon":
                assert addon in text, f"{bid}: feature {k}=addon but {addon} not emitted"
                assert has not in text, f"{bid}: {k}=addon must not also emit {has}"
                saw_addon = True
            elif v is True:
                assert has in text and addon not in text, f"{bid}: {k}=true should emit HAS not ADDON"
            else:
                assert addon not in text, f"{bid}: {k}={v!r} should not emit ADDON"
    assert saw_addon, "no board exercised an addon feature — manifest/test drift"


def test_display_boards_emit_driver_defines():
    for bid, b in BOARDS.items():
        h = g.board_info_h(bid, b)
        if b.get("display", {}).get("present"):
            assert "#define LXVEOS_HAS_DISPLAY       1" in h
            assert "LXVEOS_DISP_DRIVER" in h and "LXVEOS_DISP_W" in h and "LXVEOS_DISP_H" in h
        else:
            assert "#define LXVEOS_HAS_DISPLAY       0" in h


def test_display_pins_emitted_when_present():
    """A display board with a `pins` block emits LXVEOS_DISP_HAS_PINS 1 plus one define per line
    matching the manifest; boards without pins emit LXVEOS_DISP_HAS_PINS 0 and no pin defines."""
    for bid, b in BOARDS.items():
        d = b.get("display", {})
        if not d.get("present"):
            continue
        h = g.board_info_h(bid, b)
        pins = d.get("pins") if isinstance(d.get("pins"), dict) else {}
        if all(isinstance(pins.get(k), int) for k in ("sclk", "mosi", "cs", "dc")):
            assert "#define LXVEOS_DISP_HAS_PINS     1" in h, f"{bid}: expected HAS_PINS 1"
            for name, key in (("SCLK", "sclk"), ("MOSI", "mosi"), ("CS", "cs"), ("DC", "dc")):
                assert f"#define LXVEOS_DISP_PIN_{name}" in h and f" {pins[key]}" in h, \
                    f"{bid}: pin {name}={pins[key]} not emitted"
            bl = (d.get("backlight") or {}).get("pin")
            assert f"#define LXVEOS_DISP_PIN_BL       {bl if isinstance(bl, int) else -1}" in h, \
                f"{bid}: backlight pin not emitted"
        else:
            assert "#define LXVEOS_DISP_HAS_PINS     0" in h, f"{bid}: expected HAS_PINS 0"
            assert "LXVEOS_DISP_PIN_SCLK" not in h, f"{bid}: leaked pin defines without a pins block"


def test_cyd_pins_match_verified_pinout():
    """Guard the flagship CYD pinout against silent manifest drift (community-verified constants)."""
    h = g.board_info_h("cyd_2432S028_classic", BOARDS["cyd_2432S028_classic"])
    for line in ("#define LXVEOS_DISP_PIN_SCLK     14", "#define LXVEOS_DISP_PIN_MOSI     13",
                 "#define LXVEOS_DISP_PIN_MISO     12", "#define LXVEOS_DISP_PIN_CS       15",
                 "#define LXVEOS_DISP_PIN_DC       2", "#define LXVEOS_DISP_PIN_RST      -1",
                 "#define LXVEOS_DISP_PIN_BL       21"):
        assert line in h, f"CYD pinout drift: missing `{line}`"


def test_input_summary_emitted():
    """board_info.h carries LXVEOS_INPUT_COUNT matching the manifest and an LXVEOS_INPUT_LIST X-macro
    with one X(...) row per input device (none on headless boards)."""
    for bid, b in BOARDS.items():
        h = g.board_info_h(bid, b)
        n = len(b.get("input") or [])
        assert f"#define LXVEOS_INPUT_COUNT       {n}" in h, f"{bid}: input count {n} not emitted"
        assert "#define LXVEOS_INPUT_LIST(X)" in h, f"{bid}: input list macro missing"
        assert h.count("    X(") == n, f"{bid}: expected {n} X() rows, got {h.count('    X(')}"


def test_validation_catches_broken_board():
    bad = {"Bad-ID": {"chip": "esp32", "flash_size": "3MB",
                      "build": {"idf_target": "esp32s3", "partition_csv": "partitions/nope.csv"},
                      "ui_profile": "x", "features": {"wifi": "maybe"},
                      "display": {"present": True}}}
    errs = g.validate_manifest(bad)
    # id charset, chip!=target, bad flash size, missing csv, bad feature value, + 5 display fields
    assert len(errs) >= 8, errs
