#!/usr/bin/env python3
"""Generate LxveOS per-board build inputs from the cyd_boards.json SSOT.

Emits, for every board in the manifest:
  boards/<id>/sdkconfig.defaults   IDF_TARGET, flash/PSRAM/partition/optimize, LXVEOS_HAS_* feature gates
  boards/<id>/board_info.h         runtime constants the BSP needs (driver, w/h, runtime_probe, ui_profile)

Adding a board = one JSON edit + re-run. The manifest stays the single source shared with Cyber Controller.
No ESP-IDF required to run this (pure Python).

Boards are built with explicit idf.py flags (a private -B build dir + that board's SDKCONFIG + the chained
SDKCONFIG_DEFAULTS + an exported LXVEOS_BOARD) — NOT CMakePresets. idf.py auto-applies the FIRST preset in
any CMakePresets.json it finds (building every board against the first board's config) and never expands
${sourceDir} in a named preset's binaryDir, so presets and idf.py are mutually incompatible for a multi-board
tree. The explicit-flags form is the supported ESP-IDF multi-config path; see README / build-matrix.yml.

Usage:
  gen_board_configs.py           validate the manifest, then (re)generate boards/*
  gen_board_configs.py --check   validate the manifest only (write nothing); exit 1 on any error (CI gate).
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "cyd_boards.json")

FLASH = {"4MB": "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y",
         "8MB": "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y",
         "16MB": "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"}
# feature keys whose value is boolean True -> compile the module in (HAS_*). "addon" strings are
# left out (not compiled by default; unlocked at runtime by the capability probe).
HAS_KEYS = ("wifi", "ble", "bt_classic", "display", "ir", "gps", "subghz", "nrf24", "nfc", "storage")
# feature keys that can be an "addon" (external module on operator pins) -> CONFIG_LXVEOS_ADDON_*. The op
# catalog reports these "attachable" when not built-in. wifi/ble/bt_classic/display are soldered silicon, never
# add-ons. Must match the CONFIG_LXVEOS_ADDON_* symbols declared in components/lxveos_config/Kconfig.
ADDON_KEYS = ("storage", "gps", "ir", "subghz", "nrf24", "nfc")
# ESP32 families LxveOS targets. An idf_target outside this set can't be built by ESP-IDF v6.
KNOWN_TARGETS = ("esp32", "esp32s2", "esp32s3", "esp32c2", "esp32c3", "esp32c5", "esp32c6", "esp32h2", "esp32p4")
# Feature-map values allowed by the manifest's honesty rule: compiled-in (True), absent (False),
# or runtime-unlockable hardware add-on ("addon").
FEATURE_VALUES = (True, False, "addon")
# Fixed (non-runtime-probe) panel drivers each get a compile-time selector (LXVEOS_DISP_DRIVER_IS_<DRIVER>)
# so the BSP's create_panel() can #if on the concrete driver — a driver STRING can't be preprocessed. The
# classic CYD is a runtime probe (ILI9341 vs ST7789) and needs no selector. Add a driver here AND a
# create_panel branch when a new fixed panel gains real GPIOs. Two separate gates cover this: validate_manifest
# + the board-config test enforce driver->selector (a manifest driver missing from this list fails CI), while
# create_panel()'s #else fires a compile-time #error if a HAS_PINS board's selector has no matching branch.
# Each name must already be a valid C identifier tail (letters/digits only).
FIXED_DISPLAY_DRIVERS = ("ST7796", "ST7789V2", "AXS15231B")


def validate_manifest(boards, root=ROOT):
    """Return a list of human-readable problems with the manifest (empty == valid). Catches the classes
    that would otherwise silently emit a broken/wrong board image or only fail deep in the ESP-IDF build:
    unknown flash size (silently dropped), unknown/mismatched target, a partition CSV that doesn't exist,
    an incomplete display block, a stray feature value, and top-level vs build.psram drift."""
    errs = []
    if not isinstance(boards, dict) or not boards:
        return ["manifest 'boards' is missing or empty"]
    for bid, b in boards.items():
        p = f"[{bid}]"
        if not isinstance(bid, str) or not bid or not bid.replace("_", "").isalnum():
            errs.append(f"{p} board id must be [A-Za-z0-9_] (no spaces/dashes/dots — used in paths/CMake names)")
        if not isinstance(b, dict):
            errs.append(f"{p} board entry must be an object")
            continue
        build = b.get("build", {})
        if not isinstance(build, dict):
            errs.append(f"{p} 'build' must be an object")
            build = {}
        # chip / target
        chip = b.get("chip")
        target = build.get("idf_target", chip)
        if not chip:
            errs.append(f"{p} missing 'chip'")
        if target not in KNOWN_TARGETS:
            errs.append(f"{p} idf_target '{target}' not in {KNOWN_TARGETS}")
        if chip and target and chip != target:
            errs.append(f"{p} chip '{chip}' != build.idf_target '{target}' (must match)")
        # flash size — must map to a CONFIG or the generator silently omits it (image built at the wrong size)
        fs = b.get("flash_size")
        if fs not in FLASH:
            errs.append(f"{p} flash_size '{fs}' not in {tuple(FLASH)} (would be silently dropped)")
        # partition CSV must exist on disk
        csv = build.get("partition_csv")
        if csv and not os.path.isfile(os.path.join(root, csv)):
            errs.append(f"{p} partition_csv '{csv}' does not exist")
        # PSRAM must not disagree between the top-level flag and build.psram
        if "psram" in b and "psram" in build and bool(b["psram"]) != bool(build["psram"]):
            errs.append(f"{p} psram mismatch: top-level {b['psram']} != build.psram {build['psram']}")
        # ui_profile
        if not b.get("ui_profile"):
            errs.append(f"{p} missing 'ui_profile'")
        # features shape
        feats = b.get("features", {})
        if not isinstance(feats, dict):
            errs.append(f"{p} 'features' must be an object")
        else:
            for k, v in feats.items():
                if v not in FEATURE_VALUES:
                    errs.append(f"{p} feature '{k}'={v!r} must be one of true/false/\"addon\"")
        # display block completeness (only when a panel is present)
        d = b.get("display", {})
        if isinstance(d, dict) and d.get("present"):
            for key, ok in (("driver", bool(d.get("driver"))),
                            ("native_w", isinstance(d.get("native_w"), int) and d.get("native_w", 0) > 0),
                            ("native_h", isinstance(d.get("native_h"), int) and d.get("native_h", 0) > 0),
                            ("bus", bool(d.get("bus"))),
                            ("hal_backend", bool(d.get("hal_backend")))):
                if not ok:
                    errs.append(f"{p} display.present but '{key}' missing/invalid")
            # A fixed (non-runtime-probe) driver must have a create_panel selector, else create_panel would
            # silently fall through to the classic-CYD 0xD3 heuristic on a panel it doesn't fit.
            drv = d.get("driver") or ""
            if drv and not d.get("runtime_probe") and drv not in FIXED_DISPLAY_DRIVERS:
                errs.append(f"{p} fixed display.driver {drv!r} has no create_panel selector "
                            f"(add it to FIXED_DISPLAY_DRIVERS + a create_panel branch)")
            # optional GPIO pinout — if present it must be an object of valid pin ints (-1 = tied/none).
            pins = d.get("pins")
            if pins is not None:
                if not isinstance(pins, dict):
                    errs.append(f"{p} display.pins must be an object or null")
                else:
                    for pk, pv in pins.items():
                        if pk.startswith("_"):
                            continue
                        if not isinstance(pv, int) or pv < -1 or pv > 48:
                            errs.append(f"{p} display.pins.{pk}={pv!r} must be an int in [-1,48]")
        # input pinouts — same rule as display pins (verified ints in [-1,48]); an input's `pins` is optional.
        for it in (b.get("input") or []):
            ipins = it.get("pins") if isinstance(it, dict) else None
            if ipins is None:
                continue
            if not isinstance(ipins, dict):
                errs.append(f"{p} input '{it.get('class','?')}' pins must be an object or null")
                continue
            for pk, pv in ipins.items():
                if pk.startswith("_"):
                    continue
                if not isinstance(pv, int) or pv < -1 or pv > 48:
                    errs.append(f"{p} input '{it.get('class','?')}' pins.{pk}={pv!r} must be an int in [-1,48]")
    return errs


def sdkconfig_lines(bid, b):
    build = b.get("build", {})
    out = ['# GENERATED from cyd_boards.json by scripts/gen_board_configs.py — do not edit by hand.',
           f'# board: {bid}']
    out.append(f'CONFIG_IDF_TARGET="{build.get("idf_target", b.get("chip", "esp32"))}"')
    fs = b.get("flash_size")
    if fs in FLASH:
        out.append(FLASH[fs])
    csv = build.get("partition_csv")
    if csv:
        out.append("CONFIG_PARTITION_TABLE_CUSTOM=y")
        out.append(f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{csv}"')
    if build.get("psram"):
        out.append("CONFIG_SPIRAM=y")
        if build.get("psram_mode") == "oct":
            out += ["CONFIG_SPIRAM_MODE_OCT=y", "CONFIG_SPIRAM_SPEED_80M=y",
                    "CONFIG_SPIRAM_XIP_FROM_PSRAM=y"]
    out.append("CONFIG_COMPILER_OPTIMIZATION_PERF=y" if build.get("optimize") == "perf"
               else "CONFIG_COMPILER_OPTIMIZATION_SIZE=y")
    feats = b.get("features", {})
    for k in HAS_KEYS:
        if feats.get(k) is True:
            out.append(f"CONFIG_LXVEOS_HAS_{k.upper()}=y")
        elif feats.get(k) == "addon" and k in ADDON_KEYS:
            # An attachable external module (CC1101/nRF24/PN532/IR/GPS/SD): not compiled in, but the op
            # catalog reports it "attachable" (vs a flat "unavailable") via CONFIG_LXVEOS_ADDON_*.
            out.append(f"CONFIG_LXVEOS_ADDON_{k.upper()}=y")
    return "\n".join(out) + "\n"


def _touch_with_pins(inputs):
    """The first SPI touch controller carrying a verified pin set, or (None, None). Feeds the LXVEOS_TOUCH_*
    block the BSP uses to bring up an LVGL pointer indev; a pin-less touch entry stays HAS_PINS 0 (honesty)."""
    need = ("sclk", "mosi", "miso", "cs")
    for it in inputs:
        if not isinstance(it, dict) or it.get("class") != "touch":
            continue
        pins = it.get("pins")
        if isinstance(pins, dict) and all(isinstance(pins.get(k), int) for k in need):
            return it, pins
    return None, None


def board_info_h(bid, b):
    d = b.get("display", {})
    present = bool(d.get("present"))
    def s(v):  # C string or NULL
        return "NULL" if v is None else f'"{v}"'
    lines = ["#pragma once",
             "// GENERATED from cyd_boards.json by scripts/gen_board_configs.py — do not edit by hand.",
             f'#define LXVEOS_BOARD_ID          "{bid}"',
             f'#define LXVEOS_CHIP              "{b.get("chip","")}"',
             f'#define LXVEOS_UI_PROFILE        "{b.get("ui_profile","headless")}"',
             f'#define LXVEOS_HAS_DISPLAY       {1 if present else 0}']
    if present:
        # Compile-time driver selector for the SPI create_panel path: a fixed-driver panel can't be #if'd on
        # the driver STRING, so emit one numeric flag per known fixed driver (create_panel #if's on them). The
        # classic CYD is a runtime probe (ILI9341 vs ST7789) and leaves them all 0.
        drv = d.get("driver") or ""
        lines.append(f'#define LXVEOS_DISP_DRIVER       {s(d.get("driver"))}')
        lines += [f'#define LXVEOS_DISP_DRIVER_IS_{fd} {1 if drv == fd else 0}'
                  for fd in FIXED_DISPLAY_DRIVERS]
        lines += [
            f'#define LXVEOS_DISP_RUNTIME_PROBE {1 if d.get("runtime_probe") else 0}',
            f'#define LXVEOS_DISP_W            {d.get("native_w", 0)}',
            f'#define LXVEOS_DISP_H            {d.get("native_h", 0)}',
            f'#define LXVEOS_DISP_BUS          {s(d.get("bus"))}',
            f'#define LXVEOS_DISP_BACKEND      {s(d.get("hal_backend"))}',
        ]
        # GPIO pinout — emitted only when the manifest carries a verified `pins` block (many boards
        # still have pins=null). LXVEOS_DISP_HAS_PINS gates the esp_lcd SPI bring-up in lxveos_board;
        # -1 means the line is tied/absent (e.g. RST tied to EN). BL pin comes from display.backlight.
        pd = d.get("pins")
        pins = pd if isinstance(pd, dict) else {}
        need = ("sclk", "mosi", "cs", "dc")  # minimum to open an SPI panel-IO handle
        has_pins = all(isinstance(pins.get(k), int) for k in need)
        lines.append(f'#define LXVEOS_DISP_HAS_PINS     {1 if has_pins else 0}')
        if has_pins:
            bl = d.get("backlight") or {}
            bl_pin = bl.get("pin")
            lines += [
                f'#define LXVEOS_DISP_PIN_SCLK     {pins.get("sclk", -1)}',
                f'#define LXVEOS_DISP_PIN_MOSI     {pins.get("mosi", -1)}',
                f'#define LXVEOS_DISP_PIN_MISO     {pins.get("miso", -1)}',
                f'#define LXVEOS_DISP_PIN_CS       {pins.get("cs", -1)}',
                f'#define LXVEOS_DISP_PIN_DC       {pins.get("dc", -1)}',
                f'#define LXVEOS_DISP_PIN_RST      {pins.get("rst", -1)}',
                f'#define LXVEOS_DISP_PIN_BL       {bl_pin if isinstance(bl_pin, int) else -1}',
                # Backlight drive: LEDC PWM (dimmable) vs plain GPIO on/off, and the level that means "on"
                # (BL is active-HIGH on the CYD). The BSP configures LEDC when BL_PWM, else a push-pull GPIO.
                f'#define LXVEOS_DISP_BL_PWM       {1 if bl.get("pwm") else 0}',
                f'#define LXVEOS_DISP_BL_ACTIVE_LEVEL {1 if bl.get("active_level", 1) else 0}',
                # Panel column/row offset (esp_lcd_panel_set_gap): a controller's visible-window origin can be
                # shifted from the RAM origin (common on ST7789 cuts). 0/0 on the CYD; carried so a shifted
                # panel is a manifest edit, not a code change. col_offset/row_offset are null-safe -> 0.
                f'#define LXVEOS_DISP_GAP_X        {d.get("col_offset") or 0}',
                f'#define LXVEOS_DISP_GAP_Y        {d.get("row_offset") or 0}',
            ]
    # Input devices as an X-macro list so the board layer can iterate them at compile time:
    #   #define X(class, controller, bus, lvgl_indev) ...   then   LXVEOS_INPUT_LIST(X)
    # Empty (LXVEOS_INPUT_COUNT 0, empty list) on headless boards.
    inputs = b.get("input") or []
    lines.append(f'#define LXVEOS_INPUT_COUNT       {len(inputs)}')
    if inputs:
        rows = [f'    X("{it.get("class","")}", "{it.get("controller") or ""}", '
                f'"{it.get("bus","")}", "{it.get("maps_to_lvgl","none") or "none"}")'
                for it in inputs]
        lines.append("#define LXVEOS_INPUT_LIST(X) \\\n" + " \\\n".join(rows))
    else:
        lines.append("#define LXVEOS_INPUT_LIST(X)")
    # Touch controller pinout (parallel to the display pins): emitted only when a touch input carries a
    # verified pin set, so the BSP can bring up an XPT2046 LVGL pointer indev. LXVEOS_TOUCH_HAS_PINS gates it;
    # -1 == absent (e.g. no IRQ line). A pin-less touch entry stays HAS_PINS 0 (honesty rule).
    tit, tpins = _touch_with_pins(inputs)
    lines.append(f'#define LXVEOS_TOUCH_HAS_PINS    {1 if tpins is not None else 0}')
    if tit is not None and tpins is not None:
        # Does touch share the display's SPI bus? On the classic CYD touch is on its OWN pins (a separate
        # host), but the 3.5" CYD (ESP32-3248S035R) wires the XPT2046 onto the DISPLAY bus (same sclk/mosi/miso,
        # only cs/irq are its own). When shared, the BSP must attach touch to the already-initialised display
        # host, NOT init a second bus (double-init panic). Compare the three bus pins to the display's.
        dpins = d.get("pins") if isinstance(d.get("pins"), dict) else {}
        shares_bus = (isinstance(dpins.get("sclk"), int)
                      and all(tpins.get(k) == dpins.get(k) for k in ("sclk", "mosi", "miso")))
        lines += [
            f'#define LXVEOS_TOUCH_CONTROLLER  {s(tit.get("controller"))}',
            f'#define LXVEOS_TOUCH_SHARES_DISPLAY_BUS {1 if shares_bus else 0}',
            f'#define LXVEOS_TOUCH_PIN_SCLK    {tpins.get("sclk", -1)}',
            f'#define LXVEOS_TOUCH_PIN_MOSI    {tpins.get("mosi", -1)}',
            f'#define LXVEOS_TOUCH_PIN_MISO    {tpins.get("miso", -1)}',
            f'#define LXVEOS_TOUCH_PIN_CS      {tpins.get("cs", -1)}',
            f'#define LXVEOS_TOUCH_PIN_IRQ     {tpins.get("irq", -1)}',
        ]
    return "\n".join(lines) + "\n"


def load_boards():
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)["boards"]


def generate(boards):
    for bid, b in boards.items():
        bdir = os.path.join(ROOT, "boards", bid)
        os.makedirs(bdir, exist_ok=True)
        with open(os.path.join(bdir, "sdkconfig.defaults"), "w", encoding="utf-8", newline="\n") as f:
            f.write(sdkconfig_lines(bid, b))
        with open(os.path.join(bdir, "board_info.h"), "w", encoding="utf-8", newline="\n") as f:
            f.write(board_info_h(bid, b))
    print(f"generated {len(boards)} boards -> boards/*/")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    check = "--check" in argv
    boards = load_boards()
    errs = validate_manifest(boards)
    if errs:
        print("cyd_boards.json validation FAILED:", file=sys.stderr)
        for e in errs:
            print(f"  - {e}", file=sys.stderr)
        return 1
    if check:
        print(f"OK — manifest valid ({len(boards)} boards).")
        return 0
    generate(boards)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
