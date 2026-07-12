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


def test_display_boards_emit_driver_defines():
    for bid, b in BOARDS.items():
        h = g.board_info_h(bid, b)
        if b.get("display", {}).get("present"):
            assert "#define LXVEOS_HAS_DISPLAY       1" in h
            assert "LXVEOS_DISP_DRIVER" in h and "LXVEOS_DISP_W" in h and "LXVEOS_DISP_H" in h
        else:
            assert "#define LXVEOS_HAS_DISPLAY       0" in h


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
