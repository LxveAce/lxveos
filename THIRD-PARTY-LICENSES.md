# Third-party licenses

LxveOS core is MIT. Components and upstream work are used per the table below. Reuse mode:
**Direct-MIT-vendor** (code folded in, notice preserved) · **Clean-room** (ideas only, reimplemented) ·
**Registry-dep** (pulled unmodified from the ESP Component Registry at build time). Verified 2026-07-07;
re-verify each upstream's LICENSE at the pinned commit before bumping a version. Full detail:
command-center/projects/lxveos/{inspiration-and-reuse.md,CREDITS-DRAFT.md}.

## Upstream firmware
| Project | Author / org | License | Reuse mode |
|---|---|---|---|
| ESP32 Marauder | Just Call Me Koko | MIT | Direct-MIT-vendor |
| ESP32-DIV | CiferTech | MIT | Direct-MIT-vendor (recon only) |
| esp32-wifi-penetration-tool | risinek | MIT | Direct-MIT-vendor (capture core) |
| Evil-M5Project | 7h30th3r0n3 | MIT | Direct-MIT-vendor |
| WiFi Nugget | Alex Lynd / HakCat | MIT | Direct-MIT-vendor |
| GhostESP | GhostESP-Revival | GPL-3.0 | Clean-room (ideas only) |
| Bruce | BruceDevices | AGPL-3.0 | Clean-room (ideas only) |
| M5Stick-NEMO | Noah Axon | GPL-2.0+/3.0 | Clean-room (ideas only) |
| minigotchi-ESP32 | — | GPL-3.0 | Clean-room (ideas only) |
| esp8266_deauther | Spacehuhn | CC BY-NC 4.0 | Ethos/UX only (no code) |

## Build-time dependencies (ESP Component Registry / ESP-IDF)
| Component | License | Use |
|---|---|---|
| ESP-IDF | Apache-2.0 | SDK / build system |
| LVGL | MIT | UI toolkit |
| esp_lvgl_port, esp_lcd, esp_lcd_touch | Apache-2.0 | display/input HAL |
| esp_lcd_axs15231b, esp_lcd_touch_xpt2046 | (community — verify before vendoring) | QSPI panel / resistive touch |
| esp-bsp / esp_bsp_generic | Apache-2.0 | board support contract |
| esp-nimble | Apache-2.0 | BLE host |

Any redistributed GPL/AGPL binary (launcher-aggregation or an optional lxveos-gpl variant) ships its complete
corresponding source + build scripts, plus an in-UI "Source" link for AGPL-over-network. The MIT core does not
contain copyleft code.
