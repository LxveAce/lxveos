# LxveOS

**A unified, multi-board ESP32 security firmware — by LxveLabs, built by LxveAce.**

LxveOS is one MIT-licensed [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) codebase that runs on many
boards, with a **per-board variant tuned to each board's strengths** and features that light up only where the
hardware supports them. It builds on the great open ESP32 security firmware — Marauder, ESP32-DIV, GhostESP,
Bruce and others — with proper credit and **without competing** with them: permissively-licensed code is reused
directly, copyleft work is learned from clean-room (ideas, not code). See [`CREDITS.md`](CREDITS.md).

> ⚠️ **Status: M0 — builds clean in CI, not yet hardware-validated.** All five M0 Tier-1 boards compile against ESP-IDF
> v6.0.2 in CI (a fast host-side manifest gate, then the full 5-board build matrix). **What exists in code today:** the
> serial control console + the full operation suite — passive Wi-Fi/BLE recon, defensive detectors, wardrive CSV logging,
> and the **labelled, arm-gated offensive ops** (evil-portal/karma credential capture, BLE HID injection, sub-GHz OOK
> replay, nRF24 MouseJack, NFC UID clone) — plus a capability registry, the CYD panel-identity probe, an LVGL launcher
> shell, and ILI9341/ST7789 panel drivers. **What is NOT done:** none of it has been flashed to or bench-validated on a
> physical device yet, and on-device input/storage bring-up (and per-board display pinouts beyond the CYD) is still
> pending. Read "exists in code / CI-built" as distinct from "validated on silicon" — nothing here claims to work on a
> device until bench-validated.
> **Authorized, lawful security research & education only** — see [`RESPONSIBLE-USE.md`](RESPONSIBLE-USE.md).
> **No jammer, ever** — LxveOS builds and transmits **no** RF-jamming / deauth-flood / DoS frames in any tier; Wi-Fi
> deauth ships only as *detection*. The offensive ops above (including sub-GHz / nRF24 transmit) are **retained but
> compiled behind a two-factor `arm` gate** and are for hardware you own or are explicitly authorized to test — see
> [`RESPONSIBLE-USE.md`](RESPONSIBLE-USE.md).

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
| `sniff [seconds] [channel]` | passive Wi-Fi packet monitor — promiscuous, tally frames by 802.11 type + channel (counts only, no payloads); channel-hops by default, or pass a channel `1-13` to lock/dwell on one; transmits nothing |
| `stations [seconds] [channel]` | passive client-station scan — infer client↔AP links from data-frame addresses, list clients with ESSID / frames / RSSI; channel-hops by default, or pass a channel `1-13` to dwell on a known AP's channel; transmits nothing |
| `probes [seconds] [channel]` | passive probe-request SSID logger — list the SSIDs nearby client devices are actively hunting for (each directed probe names a network the device saved/connected to before — a recon signal + privacy leak), with per-SSID seen-count and RSSI + a wildcard-probe tally; channel-hops by default, or lock to a channel `1-13`; listen only, never sends a probe response |
| `capture [seconds] [channel]` | passive EAPOL/PMKID capture — detect WPA 4-way-handshake messages and emit hashcat-22000 lines: a `WPA*01` line per RSN PMKID **and** a `WPA*02` line per captured handshake — pairing an ANONCE source (M1/M3) with an EAPOL+MIC source (M2/M4) by replay counter: M1+M2 (`MESSAGEPAIR 00`), M3+M2 (`02`, when M1 was missed), or M3+M4 (`05`, when neither M1 nor M2 was seen); all feed the Cyber Controller crack pipeline; channel-hops by default, or pass a channel `1-13` to dwell on a known AP's channel; listen only, never forces a handshake |
| `defend [seconds] [channel]` | passive deauth/disassoc detector — count deauthentication/disassociation frames (deauth-attack fingerprint) + name the busiest source; channel-hops by default, or pass a channel `1-13` to dwell on a known AP's channel; listen only, sends nothing |
| `eviltwin` | passive evil-twin / rogue-AP detector (custom) — flag any ESSID advertised by multiple BSSIDs or by both an open and an encrypted BSSID (karma/Pineapple signature); listen only |
| `apaudit` | passive AP security-posture auditor (custom) — grade every AP's encryption and flag the weak ones: open (no encryption), WEP (broken cipher), legacy WPA (deprecated TKIP); **also flags any AP advertising WPS** (Wi-Fi Protected Setup — its PIN is brute-forceable to recover the PSK even on WPA2/WPA3, so a `[W]` marks WPS-on-otherwise-strong APs), reading ESP-IDF's parsed `wifi_ap_record_t.wps` bit; prints a per-grade tally (open/WEP/WPA/WPA2/WPA3) + WPS + hidden-SSID counts over one passive scan; listen only, sends nothing |
| `wardrive` | passive Wi-Fi wardrive CSV export — one passive scan → a machine-importable `bssid,ssid,channel,rssi,auth,hidden` CSV (SSID quoted/escaped) for a host mapping/inventory tool; GPS-less (the host adds coordinates); listen only |
| `airspace [ble_seconds]` | airspace occupancy summary (custom) — one passive Wi-Fi scan + (when BLE is active) a short passive BLE observe → a quick "what's around me": AP count with open / WPS-exposed / hidden splits, and BLE advertiser count with known-tracker count; emits a single `LXVEOS/1 snapshot` event for the Cyber Controller dashboard; listen only, sends nothing |
| `blescan [seconds]` | passive BLE device scan — NimBLE GAP observe (passive, never sends a scan request); list nearby advertisers with address / type / RSSI / adv-flags / vendor (from manufacturer/Fast-Pair data) / local name + GAP appearance (Phone / Watch / Earbuds / Keyboard / …), plus a per-device line of any advertised 16-bit service-class UUIDs — named when known (Battery / HeartRate / HID / FastPair / Eddystone / …), else raw `0xNNNN`. This scan is passive (never advertises); the non-connectable broadcaster role stays compiled out (no BLE advert-spam), while a connectable keyboard advert exists only under the arm-gated `badble` op |
| `bleflood [seconds]` | passive BLE advertisement-flood / spam detector (custom) — measures advertiser churn (BLE-spam attacks rotate through many random addresses) and flags a likely flood; reports total adverts / unique advertisers / busiest source + a per-vendor payload breakdown (Apple / Microsoft / Google / Samsung / Fast-Pair) that attributes a flood to a vendor; listen only |
| `btracker [seconds]` | passive BLE item-tracker / stalking detector (custom) — flags advertisers matching a known item-tracker signature: Apple Find My / AirTag (Apple mfg type `0x12`), Tile (`0xFEED`), Samsung SmartTag (`0xFD5A`), Chipolo (`0xFE33`), PebbleBee (`0xFA25`), and Google Find My Network (`0xFEAA` service-data frame `0x40`, distinguished from Eddystone) — all verified against the [AirGuard](https://github.com/seemoo-lab/AirGuard) anti-stalking project. A tracker that follows you over time/place but isn't yours is the AirTag-stalking signal; re-run as you move. Listen only |
| `blehid [seconds]` | **defense** — flag nearby BLE HID devices (rogue-keyboard / injector detector); listen only |
| `arm` · `arm <token>` · `arm status` | two-factor enable for offensive-TX ops — `arm` requests a one-time token (30 s window), `arm <token>` confirms, `arm status` reports the current state (safe/pending/armed) without changing it; an armed session auto-disarms after 120 s idle |
| `disarm` | hard-disarm — return to SAFE (offensive TX not permitted) |
| `evilportal [ssid\|karma\|template <id>\|templates\|creds\|stop]` | **offensive (needs `arm`)** — rogue AP + captive credential-capture portal (DNS auto-pop, retained-credential readout) |
| `badble "<duckyscript>" \| stop \| status` | **offensive (needs `arm`)** — BLE HID keystroke injection ("BadBLE") |
| `ir recv <rx_gpio> [s] \| send <tx_gpio> \| show` | IR capture + replay via RMT (universal-remote); `send` transmits a captured frame. Needs an IR receiver/emitter |
| `subghz begin <sclk> <miso> <mosi> <cs> \| rssi <mhz> \| capture <gdo0> <mhz> [s] \| replay <gdo0> \| end` | CC1101 sub-GHz — RSSI + OOK capture (receive) plus **arm-gated `replay`** (offensive OOK re-emit). Add-on module |
| `nrf24 begin <sck> <miso> <mosi> <csn> <ce> \| scan \| sniff \| mousejack <text> \| end` | nRF24L01+ 2.4 GHz — channel scan + address sniff (receive) plus **arm-gated `mousejack`** keystroke injection. Add-on module |
| `nfc begin <sda> <scl> \| read [seconds] \| clone <8hexUID> \| end` | PN532 NFC — read a card UID (receive) plus **arm-gated `clone`** (write a spoofed UID to a Gen2 magic card). Add-on module |
| `sysinfo` | ESP-IDF version, reset reason, boot count, uptime, heap free |
| `status` | one machine-readable line for the Cyber Controller host (below) |
| `bridge` · `bridge on\|off\|status` | toggle machine-readable `LXVEOS/1 <type> k=v` event emission for the Cyber Controller bridge (off by default; recon/defense/capture/arm ops stream typed event lines when on) — see `docs/EVENT-PROTOCOL.md` |
| `loglevel <tag\|*> <level>` | change ESP-IDF log verbosity at runtime |
| `nvs get\|set <key> [value]` | small persistent key/value store for operator settings |
| `reboot` | restart the unit |

**Offensive-TX safety (the `arm` gate).** Recon, defensive, and logging ops transmit nothing. The offensive ops
(`evilportal`, `badble`, `subghz replay`, `nrf24 mousejack`, `nfc clone`) are compiled in by default but every one is
gated the same way: nothing goes on-air until you `arm` (request a one-time token, then confirm it within 30 s), and an
armed session auto-disarms after 120 s of inactivity or an explicit `disarm`. A conservative or public build can define
`LXVEOS_TX_DISABLE` to strip the offensive emitters out entirely (then `arm` always refuses). LxveOS still authors **no**
jammer / deauth-flood / DoS transmit frames — those are catalogued and labelled but never emitted.

Three facts persist in NVS across boots: the resolved CYD panel identity, the authorized-use acceptance, and a lifetime
boot counter.

**Cyber Controller bridge (seed).** `status` prints one parseable line that the
[Cyber Controller](https://github.com/LxveAce/cyber-controller) host can read to identify a unit — a stable versioned
prefix plus space-separated `key=value` fields (safe slugs / hex capability mask / decimal, no embedded spaces):
```
LXVEOS/1 status board=<id> chip=<esp32|esp32s3> ui=<profile> fw=<version> panel=<driver|none> caps=0x<hex> ops=<ready>/<planned>/<unavailable> heap=<bytes> arm=<safe|pending|armed> tx=<0|1>
```
The `arm=` field lets the host see whether a unit currently has offensive TX armed; `tx=` reports whether offensive TX is
compiled into the build at all (`tx=0` on a `LXVEOS_TX_DISABLE` image), so the host can tell a TX-capable-but-safe unit from
one that can never arm. Fields are appended over time; a host parser keys on field names, so older hosts ignore any field
they don't know.

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
