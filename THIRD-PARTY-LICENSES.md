# Third-party licenses

LxveOS core is MIT. Components and upstream work are used per the table below. Reuse mode:
**Direct-MIT-vendor** (code folded in, notice preserved) · **Clean-room** (ideas only, reimplemented) ·
**Registry-dep** (pulled unmodified from the ESP Component Registry at build time). Verified 2026-07-07;
re-verify each upstream's LICENSE at the pinned commit before bumping a version.

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
| esp_lcd_ili9341 | Apache-2.0 | ILI9341 SPI panel driver (CYD build) |
| esp_lcd_axs15231b, esp_lcd_touch_xpt2046 | (community — verify before vendoring) | QSPI panel / resistive touch |
| esp-bsp / esp_bsp_generic | Apache-2.0 | board support contract |
| esp-nimble | Apache-2.0 | BLE host |

## Verbatim MIT notices (Direct-MIT-vendor)

MIT requires the copyright notice to travel with any reused code. Where LxveOS folds in MIT-licensed work, the
upstream notice is retained here.

### ESP32 Marauder — Just Call Me Koko

Ported code: the passive Pwnagotchi-presence detector (fixed grid MAC + JSON-in-SSID identity parse),
following Marauder's "Detect Pwnagotchi" menu.

```
MIT License

Copyright (c) 2020 Just Call Me Koko

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Copyleft handling

Any redistributed GPL/AGPL binary (launcher-aggregation or an optional lxveos-gpl variant) ships its complete
corresponding source + build scripts, plus an in-UI "Source" link for AGPL-over-network. The MIT core does not
contain copyleft code.
