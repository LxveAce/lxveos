# LxveOS

**A unified, multi-board ESP32 security firmware — by LxveLabs, built by LxveAce.**

LxveOS is one MIT-licensed [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) codebase that runs on many
boards, with a **per-board variant tuned to each board's strengths** and features that light up only where the
hardware supports them. It builds on the great open ESP32 security firmware — Marauder, ESP32-DIV, GhostESP,
Bruce and others — with proper credit and **without competing** with them: permissively-licensed code is reused
directly, copyleft work is learned from clean-room (ideas, not code). See [`CREDITS.md`](CREDITS.md).

> ⚠️ **Status: M0 — builds clean in CI, not yet hardware-validated.** All five M0 Tier-1 boards compile against ESP-IDF
> v6.0.2 in CI (a fast host-side manifest gate, then the full 5-board build matrix). What runs today is the core: board
> bring-up, a capability registry, the CYD runtime panel-identity resolver, and a serial control console (see below). The
> firmware has **not** been flashed to or bench-validated on a physical device yet, and on-device display/input/storage
> bring-up is still pending — that needs each board's verified GPIO pinout, which isn't captured in the manifest yet.
> Nothing here claims to work on a device until bench-validated.
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

## Serial control surface (M0)
Every board — headless or not — boots into a serial console (`esp_console` REPL) over its default UART / USB-Serial-JTAG.
Because this is security tooling, the console is **locked on first boot** until the operator types `agree` to accept the
authorized-use terms; the acceptance is stored in NVS, so it's a one-time gate per unit, not a per-boot prompt.

| command | what it does |
| --- | --- |
| `help` | list commands |
| `agree` | accept the authorized-use terms and unlock the console |
| `info` | firmware version, board id, chip, UI profile |
| `caps` | capability registry — which subsystems (wifi / ble / storage / …) are active |
| `features` | operation catalog — the security ops (Wi-Fi scan/monitor/capture, BLE, sub-GHz, …) this unit can run or has planned, each gated by capability |
| `scan` | passive Wi-Fi AP scan — listen for beacons and list APs in range (SSID / RSSI / channel / auth / BSSID); transmits nothing |
| `sniff [seconds]` | passive Wi-Fi packet monitor — promiscuous channel-hop, tally frames by 802.11 type + channel (counts only, no payloads); transmits nothing |
| `stations [seconds]` | passive client-station scan — infer client↔AP links from data-frame addresses, list clients with ESSID / frames / RSSI; transmits nothing |
| `capture [seconds]` | passive EAPOL/PMKID capture — detect WPA 4-way-handshake messages + emit a hashcat-22000 `WPA*01` line per PMKID (feeds the Cyber Controller crack pipeline); listen only, never forces a handshake |
| `defend [seconds]` | passive deauth/disassoc detector — count deauthentication/disassociation frames (deauth-attack fingerprint) + name the busiest source; listen only, sends nothing |
| `eviltwin` | passive evil-twin / rogue-AP detector (custom) — flag any ESSID advertised by multiple BSSIDs or by both an open and an encrypted BSSID (karma/Pineapple signature); listen only |
| `sysinfo` | ESP-IDF version, reset reason, boot count, uptime, heap free |
| `status` | one machine-readable line for the Cyber Controller host (below) |
| `loglevel <tag\|*> <level>` | change ESP-IDF log verbosity at runtime |
| `nvs get\|set <key> [value]` | small persistent key/value store for operator settings |
| `reboot` | restart the unit |

Three facts persist in NVS across boots: the resolved CYD panel identity, the authorized-use acceptance, and a lifetime
boot counter.

**Cyber Controller bridge (seed).** `status` prints one parseable line that the
[Cyber Controller](https://github.com/LxveAce/cyber-controller) host can read to identify a unit — a stable versioned
prefix plus space-separated `key=value` fields (safe slugs / hex capability mask / decimal, no embedded spaces):
```
LXVEOS/1 status board=<id> chip=<esp32|esp32s3> ui=<profile> fw=<version> panel=<driver|none> caps=0x<hex> ops=<ready>/<planned>/<unavailable> heap=<bytes>
```

## Build (once you have ESP-IDF v6.0.x)
```sh
python scripts/gen_board_configs.py        # cyd_boards.json -> boards/*/ (sdkconfig.defaults + board_info.h)

# Build a board with explicit idf.py flags — the supported ESP-IDF multi-config form. Each board gets a
# private -B build dir and its own SDKCONFIG; the chained SDKCONFIG_DEFAULTS ends with the generated board
# file (which sets CONFIG_IDF_TARGET, so ESP-IDF auto-applies sdkconfig.defaults.<target>). LXVEOS_BOARD is
# EXPORTED, not -D'd: ESP-IDF re-runs each component's CMakeLists in a cacheless `cmake -P` process to resolve
# requirements, where a -D cache var is invisible but the environment is inherited.
# No CMakePresets.json: idf.py auto-applies the FIRST preset in one (building every board against the first
# board's config) and never expands ${sourceDir} in a named preset's binaryDir — presets and idf.py can't
# coexist for a multi-board tree.
export B=bare_esp32_headless   # any board id in cyd_boards.json
export LXVEOS_BOARD=$B
idf.py -B build/$B -D SDKCONFIG=build/$B/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$B/sdkconfig.defaults" build
idf.py -B build/$B flash monitor
```
You can validate the manifest + generated configs **without ESP-IDF** (pure Python):
```sh
python scripts/gen_board_configs.py --check   # manifest valid (CI gate; writes nothing)
python -m pytest tests/                        # board-config pipeline unit tests
```
CI (`.github/workflows/build-matrix.yml`) runs that fast host-side `validate` gate first, then builds all five
M0 boards with ESP-IDF v6.0.2 — currently green. Still TODO on the build side: a flash/IRAM `size` gate and
on-device (`pytest-embedded` / Unity) tests once hardware bring-up lands.

## License
MIT © 2026 LxveAce / LxveLabs. Third-party components retain their own licenses — see `THIRD-PARTY-LICENSES.md`.
