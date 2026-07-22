"""Host-side (no-ESP-IDF) tests for the board-config pipeline: cyd_boards.json -> gen_board_configs.py.

The manifest is the SSOT shared with Cyber Controller, and the per-board build inputs are generated from
it. A manifest typo (unknown flash size, missing partition CSV, target/chip mismatch, incomplete display
block) would otherwise only surface deep in the ESP-IDF build — or silently produce a wrong image. These
run in CI before the build matrix as a fast gate, and locally with `python -m pytest`.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import gen_board_configs as g  # noqa: E402

BOARDS = json.loads((ROOT / "cyd_boards.json").read_text(encoding="utf-8"))["boards"]

# The M0 Tier-1 fleet the manifest + README + CI matrix commit to. Kept EQUAL to all three by
# test_m0_tier1_fleet_is_consistent (below) — a board added to one place but not the others fails there.
M0_TIER1 = {"bare_esp32_headless", "cyd_2432S028_classic", "cyd_3248S035_r",
            "m5stickc_plus2", "m5cardputer_v1", "jc3248w535_s3_qspi"}


def test_manifest_is_valid():
    errs = g.validate_manifest(BOARDS)
    assert errs == [], "cyd_boards.json validation failed:\n" + "\n".join(errs)


def _ci_matrix_boards() -> set[str]:
    """Board ids the CI build matrix builds (`- { board: <id>, target: … }` rows in build-matrix.yml)."""
    text = (ROOT / ".github" / "workflows" / "build-matrix.yml").read_text(encoding="utf-8")
    return set(re.findall(r"\bboard:\s*([A-Za-z0-9_]+)", text))


def _readme_tier1_boards() -> set[str]:
    """Board ids the README's `## M0 Tier-1 boards` section lists (they're the only backtick-quoted tokens
    in that section). Reads the section verbatim so a README edit that adds/drops a board is caught."""
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    m = re.search(r"^##\s*M0 Tier-1 boards\s*$(.*?)(?=^##\s|\Z)", text, re.DOTALL | re.MULTILINE)
    section = m.group(1) if m else ""
    return set(re.findall(r"`([A-Za-z0-9_]+)`", section))


def test_m0_tier1_fleet_is_consistent():
    """The M0 Tier-1 fleet is declared in four places — M0_TIER1 here, the board manifest
    (cyd_boards.json), the CI build matrix, and the README. They must be EQUAL, not merely overlapping, so
    a board added to (or dropped from) one place but not the others fails here in seconds rather than
    drifting silently — the failure names exactly which side is out of sync."""
    manifest = set(BOARDS)
    ci = _ci_matrix_boards()
    readme = _readme_tier1_boards()
    assert manifest == M0_TIER1, \
        f"manifest ≠ M0_TIER1: only-manifest={manifest - M0_TIER1}, only-M0_TIER1={M0_TIER1 - manifest}"
    assert ci == M0_TIER1, \
        f"CI matrix ≠ M0_TIER1: only-CI={ci - M0_TIER1}, only-M0_TIER1={M0_TIER1 - ci}"
    assert readme == M0_TIER1, \
        f"README ≠ M0_TIER1: only-README={readme - M0_TIER1}, only-M0_TIER1={M0_TIER1 - readme}"


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


def test_fixed_display_drivers_have_a_selector():
    """Every present, non-runtime-probe display driver has a matching LXVEOS_DISP_DRIVER_IS_<DRIVER>=1 selector
    and every other selector is 0; a runtime-probe board leaves them all 0. Catches a new fixed panel added to
    the manifest without a selector define — the rank-8 latent fall-through where create_panel would otherwise
    silently run the classic-CYD 0xD3 heuristic on a panel it doesn't fit. (A listed driver that has a selector
    but no create_panel() branch is caught separately, at C-compile, by create_panel()'s #else #error.)"""
    for bid, b in BOARDS.items():
        d = b.get("display", {})
        if not d.get("present"):
            continue
        h = g.board_info_h(bid, b)
        drv = d.get("driver") or ""
        probe = bool(d.get("runtime_probe"))
        for fd in g.FIXED_DISPLAY_DRIVERS:
            want = 1 if (not probe and drv == fd) else 0
            assert f"#define LXVEOS_DISP_DRIVER_IS_{fd} {want}" in h, \
                f"{bid}: expected IS_{fd} {want} for driver {drv!r} (runtime_probe={probe})"
        if not probe:
            # A fixed board's driver MUST be one of the known selectors (else create_panel has no branch and
            # its #else fires a compile #error). validate_manifest enforces this too — pinned here as well.
            assert drv in g.FIXED_DISPLAY_DRIVERS, \
                f"{bid}: fixed driver {drv!r} not in FIXED_DISPLAY_DRIVERS (create_panel would fall through)"


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


def test_cyd_backlight_and_gap_defines():
    """The CYD emits its backlight drive (PWM, active-HIGH) and panel gap (0/0) so the BSP configures LEDC
    dimming + esp_lcd_panel_set_gap from the manifest rather than hard-coded constants."""
    h = g.board_info_h("cyd_2432S028_classic", BOARDS["cyd_2432S028_classic"])
    for line in ("#define LXVEOS_DISP_BL_PWM       1",
                 "#define LXVEOS_DISP_BL_ACTIVE_LEVEL 1",
                 "#define LXVEOS_DISP_GAP_X        0",
                 "#define LXVEOS_DISP_GAP_Y        0"):
        assert line in h, f"CYD backlight/gap drift: missing `{line}`"


def test_backlight_gap_defines_track_pins():
    """The backlight-drive + gap defines are emitted exactly when the pin block is (they sit inside the same
    has-pins guard the BSP compiles against); a display board without verified pins emits none of them."""
    bl_gap = ("LXVEOS_DISP_BL_PWM", "LXVEOS_DISP_BL_ACTIVE_LEVEL", "LXVEOS_DISP_GAP_X", "LXVEOS_DISP_GAP_Y")
    for bid, b in BOARDS.items():
        h = g.board_info_h(bid, b)
        if "#define LXVEOS_DISP_HAS_PINS     1" in h:
            for d in bl_gap:
                assert f"#define {d}" in h, f"{bid}: has pins but missing {d}"
        else:
            for d in bl_gap:
                assert d not in h, f"{bid}: emitted {d} without a pins block"


def test_cyd_touch_pins_emitted():
    """The CYD's XPT2046 touch controller emits a LXVEOS_TOUCH_* block (controller + the verified separate-bus
    pinout) so the BSP can bring up a pointer indev; the pins match the community-verified manifest values."""
    h = g.board_info_h("cyd_2432S028_classic", BOARDS["cyd_2432S028_classic"])
    for line in ("#define LXVEOS_TOUCH_HAS_PINS    1",
                 '#define LXVEOS_TOUCH_CONTROLLER  "XPT2046"',
                 "#define LXVEOS_TOUCH_PIN_SCLK    25",
                 "#define LXVEOS_TOUCH_PIN_MOSI    32",
                 "#define LXVEOS_TOUCH_PIN_MISO    39",
                 "#define LXVEOS_TOUCH_PIN_CS      33",
                 "#define LXVEOS_TOUCH_PIN_IRQ     36"):
        assert line in h, f"CYD touch pinout drift: missing `{line}`"


def test_cyd_35_st7796_and_shared_bus_touch():
    """The 3.5" CYD (ESP32-3248S035R) is a FIXED ST7796 panel (no runtime probe) whose XPT2046 touch SHARES the
    display's SPI bus — both are compile-time flags the BSP branches on (create_panel picks ST7796; create_touch
    attaches to the display host instead of init-ing a 2nd bus). Regression for the board-add."""
    h = g.board_info_h("cyd_3248S035_r", BOARDS["cyd_3248S035_r"])
    for line in ('#define LXVEOS_DISP_DRIVER       "ST7796"',
                 "#define LXVEOS_DISP_DRIVER_IS_ST7796 1",
                 "#define LXVEOS_DISP_RUNTIME_PROBE 0",
                 "#define LXVEOS_DISP_W            320",
                 "#define LXVEOS_DISP_H            480",
                 "#define LXVEOS_DISP_PIN_BL       27",
                 "#define LXVEOS_TOUCH_SHARES_DISPLAY_BUS 1",
                 "#define LXVEOS_TOUCH_PIN_CS      33",
                 "#define LXVEOS_TOUCH_PIN_IRQ     36"):
        assert line in h, f"cyd_3248S035_r config drift: missing `{line}`"
    # Shared-bus means the touch's SCLK/MOSI/MISO equal the display's (only cs/irq differ).
    assert "#define LXVEOS_TOUCH_PIN_SCLK    14" in h and "#define LXVEOS_DISP_PIN_SCLK     14" in h


def test_classic_cyd_unaffected_by_st7796_addition():
    """Backward-compat: the classic 2.8" CYD stays a runtime-probe board with its OWN touch bus — the new
    ST7796 / shared-bus flags are both 0, so adding the 3.5" board didn't change the classic's bring-up."""
    h = g.board_info_h("cyd_2432S028_classic", BOARDS["cyd_2432S028_classic"])
    assert "#define LXVEOS_DISP_DRIVER_IS_ST7796 0" in h
    assert "#define LXVEOS_TOUCH_SHARES_DISPLAY_BUS 0" in h
    assert "#define LXVEOS_DISP_RUNTIME_PROBE 1" in h


def test_touch_block_absent_without_verified_pins():
    """A board with no touch controller (or a pin-less touch entry) emits LXVEOS_TOUCH_HAS_PINS 0 and no touch
    pin defines (honesty rule — the generator never invents a touch pinout)."""
    for bid, b in BOARDS.items():
        _tit, tpins = g._touch_with_pins(b.get("input") or [])
        h = g.board_info_h(bid, b)
        if tpins is None:
            assert "#define LXVEOS_TOUCH_HAS_PINS    0" in h, f"{bid}: expected TOUCH_HAS_PINS 0"
            assert "LXVEOS_TOUCH_PIN_SCLK" not in h, f"{bid}: leaked touch pins without a verified pin set"


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


def test_validation_catches_fixed_driver_without_selector():
    """A present, fixed (non-runtime-probe) display driver that isn't in FIXED_DISPLAY_DRIVERS is rejected —
    it would otherwise reach create_panel()'s #else and fail the build with a compile #error (or, before the
    guard, silently mis-probe). A runtime-probe board with the same 'unknown' driver string is fine."""
    base = {"chip": "esp32", "flash_size": "4MB",
            "build": {"idf_target": "esp32", "partition_csv": "partitions/default.csv"}, "ui_profile": "x"}
    fixed = {"B": {**base, "display": {"present": True, "driver": "ZZ9999", "native_w": 100, "native_h": 100,
                                       "bus": "spi", "hal_backend": "esp_lcd", "runtime_probe": False}}}
    assert any("no create_panel selector" in e for e in g.validate_manifest(fixed)), g.validate_manifest(fixed)
    probe = {"B": {**base, "display": {"present": True, "driver": "A|B", "native_w": 100, "native_h": 100,
                                       "bus": "spi", "hal_backend": "esp_lcd", "runtime_probe": True}}}
    assert not any("no create_panel selector" in e for e in g.validate_manifest(probe))
