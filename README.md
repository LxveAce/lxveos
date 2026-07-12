# LxveOS

**A unified, multi-board ESP32 security firmware — by LxveLabs, built by LxveAce.**

LxveOS is one MIT-licensed [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) codebase that runs on many
boards, with a **per-board variant tuned to each board's strengths** and features that light up only where the
hardware supports them. It builds on the great open ESP32 security firmware — Marauder, ESP32-DIV, GhostESP,
Bruce and others — with proper credit and **without competing** with them: permissively-licensed code is reused
directly, copyleft work is learned from clean-room (ideas, not code). See [`CREDITS.md`](CREDITS.md).

> ⚠️ **Status: M0 scaffold (unbuilt).** This repo is the structural skeleton for milestone M0. It has not yet been
> compiled against a toolchain or flashed to hardware. Nothing here claims to work on a device until bench-validated.
> **Authorized, lawful security research & education only** — see [`RESPONSIBLE-USE.md`](RESPONSIBLE-USE.md).
> **No jammer, ever** — deauth ships as detection; nRF24/CC1101 are receive/analyze only.

## Design (single source of truth)
The full design lives in `LxveAce/command-center` → `projects/lxveos/`:
- `lxveos-ARCHITECTURE.md` — the overall design + deep CYD autodetect + licensing plan
- `build-architecture.md` — this repo's ESP-IDF v6 architecture (what's implemented here)
- `board-support-matrix.md` — the 40+ board fleet, tiered M0/M1/M2
- `feature-matrix.md` — feature modules × boards + hardware add-on unlock map
- `inspiration-and-reuse.md` — per-firmware idea/license/reuse map
- `cyd_boards.json` — the board+display+input capability manifest (SSOT; a copy lives here, kept in sync)

## Core decisions
- **Base:** ESP-IDF v6.0.x + CMake. **License:** MIT (core). **UI stack:** `esp_lcd` + `esp_lvgl_port` + LVGL 9
  (LovyanGFX as a wrapped fallback backend); board interface follows the Espressif `esp-bsp` contract.
- **One codebase, board support as data:** each board is a row in `cyd_boards.json`; `scripts/gen_board_configs.py`
  turns it into the per-board build inputs under `boards/`. Adding a board = one JSON edit.
- **Flagship:** the ILI9341(1-USB) vs ST7789(2-USB) CYD panels are electrically identical → chip/features/partitions
  are compile-time, **panel identity is resolved at boot** (probe) and cached to NVS.

## M0 Tier-1 boards
`cyd_2432S028_classic` (runtime ILI9341/ST7789 probe) · `bare_esp32_headless` · `m5stickc_plus2` ·
`m5cardputer_v1` · `jc3248w535_s3_qspi`. Spans classic+S3 / none–2MB–8MB PSRAM / SPI+QSPI / resistive+capacitive
+ matrix-keyboard + buttons + headless.

## Build (once you have ESP-IDF v6.0.x)
```sh
python scripts/gen_board_configs.py        # cyd_boards.json -> boards/*, CMakePresets, board_info.h

# Build a board. idf.py doesn't expand CMakePresets ${sourceDir} macros, so pass the inputs
# explicitly (the generated board sdkconfig sets CONFIG_IDF_TARGET; ESP-IDF auto-applies
# sdkconfig.defaults.<target>). LXVEOS_BOARD is EXPORTED, not -D'd: ESP-IDF re-runs each
# component's CMakeLists in a cacheless `cmake -P` process to resolve requirements, where a -D
# cache var is invisible but the environment is inherited. CMakePresets.json (whose per-board
# "environment" sets the same var) is kept for `cmake --preset` users.
export B=bare_esp32_headless   # any board id in cyd_boards.json
export LXVEOS_BOARD=$B
idf.py -B build/$B -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$B/sdkconfig.defaults" build
idf.py -B build/$B flash monitor
```
You can validate the manifest + generated configs **without ESP-IDF** (pure Python):
```sh
python scripts/gen_board_configs.py --check   # manifest valid + CMakePresets.json in sync
python -m pytest tests/                        # board-config pipeline unit tests
```
CI (`.github/workflows/build-matrix.yml`) runs that fast host-side `validate` gate first, then builds every
board with ESP-IDF v6.0.2. (The ESP-IDF build stage is still a scaffold — not yet toolchain-verified; a
flash/IRAM size gate is a TODO.)

## License
MIT © 2026 LxveAce / LxveLabs. Third-party components retain their own licenses — see `THIRD-PARTY-LICENSES.md`.
