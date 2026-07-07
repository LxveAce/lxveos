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
idf.py --preset bare_esp32_headless build   # or any board preset
idf.py --preset bare_esp32_headless flash monitor
```
CI (`.github/workflows/build-matrix.yml`) builds every board with `idf-build-apps` + a flash/IRAM size gate.

## License
MIT © 2026 LxveAce / LxveLabs. Third-party components retain their own licenses — see `THIRD-PARTY-LICENSES.md`.
