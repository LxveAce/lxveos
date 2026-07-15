# LxveOS event protocol — the machine-readable serial stream for Cyber Controller

**Producer:** the firmware (`lxveos`). **Consumer:** CC's `src/protocols/lxveos.py`.
**Status:** extends `LXVEOS-CC-CONTROL-SPEC.md` §3 (the `status` line) to recon/defense/capture/arm output.

## Framing
Every machine line reuses the `status`-line framing so CC parses them with one tokenizer:

```
LXVEOS/1 <type> <key>=<value> <key>=<value> ...\n
```

- `LXVEOS/1` — protocol tag + version. CC verifies this prefix, then reads `<type>` and the `key=value` pairs.
- Tokens split on **single spaces**; values never contain a space, newline, or control byte. Free-text /
  arbitrary-byte fields (SSIDs, device names) are therefore **hex-encoded** (`ssid=4d794e6574`), built by
  `lxveos_evt_kv_hex()`. Numeric/enum fields are plain (`ch=6`, `rssi=-42`, `auth=wpa2`).
- Forward-compatible: CC ignores unknown `<type>`s and unknown keys, so new events/fields never break an older CC.

Machine lines are emitted **alongside** the normal human console output. A human reading the console sees the extra
`LXVEOS/1 …` lines; CC filters for the prefix.

## Enabling — the `bridge` command
Event emission is **off by default** (keeps the interactive console clean). CC turns it on after connect:

| Command | Effect |
|---|---|
| `bridge on` | Enable `LXVEOS/1 <event>` emission for recon/defense/capture/arm ops. Emits `LXVEOS/1 bridge state=on`. |
| `bridge off` | Disable it. Emits `LXVEOS/1 bridge state=off`. |
| `bridge` / `bridge status` | Print current state (`LXVEOS/1 bridge state=on|off`). |

The `status` line (LXVEOS-CC-CONTROL-SPEC §3) is **always** available regardless of `bridge` state — it is the
dashboard poll, not an event.

## Event catalog
`hex` = hex-encoded (arbitrary bytes). Sources are the CLI commands in LXVEOS-CC-CONTROL-SPEC §5.

| type | fields | source | notes |
|---|---|---|---|
| `bridge` | `state=on\|off` | `bridge` | emission toggle ack |
| `ap` | `bssid`(mac) `ssid`(hex) `ch` `rssi` `auth` | `scan` | one per AP found |
| `sta` | `mac`(mac) `ap`(mac) `rssi` `frames` `essid`(hex) | `stations` | client station; `ap` = associated BSSID, `essid` its ESSID if a beacon was also seen, `frames` = data frames observed |
| `probe` | `ssid`(hex) `seen` `rssi` | `probes` | one per directed SSID a nearby device is hunting for; `seen` = probe-request frames. No client MAC — the passive probe scan aggregates by SSID, not by device |
| `ble` | `addr`(mac) `type` `rssi` `name`(hex) `company` `fp` `appr` `tracker` | `blescan` | addr is MSB-first; `company`/`fp`/`appr`/`tracker`/`name` present only when the advert carried them. `company` = numeric Bluetooth-SIG company ID (names can contain spaces); `tracker` = item-tracker class (0 = none) |
| `hs` | `kind=pmkid\|eapol` `line`(hashcat-22000) | `capture` | one per crackable artifact; `line` is the ready-to-crack WPA*01/WPA*02 line (token-safe, ESSID hex inside), forwarded straight to Crack Lab |
| `pcap` | `id` `bytes` | `pcap_log` | a pcap segment was written (needs storage; HW) |
| `arm` | `state=safe\|pending\|armed\|tx_disabled` `token`(pending only) `window`(s, pending) | `arm`/`disarm` | arm state change. `tx_disabled` = offensive TX compiled out (LXVEOS_TX_DISABLE). Also printed as human prose (always), so CC tracks arm state even with the bridge off |
| `alert` | `kind=deauth\|eviltwin\|weak\|wps\|tracker\|bleflood\|blehid\|watch` `...`(kind-specific) | `defend`/`eviltwin`/`apaudit`/`bleflood`/`btracker`/`blehid`/`watch` | a detector fired |
| `snapshot` | `aps` `open` `wps` `bles` `trackers` | `airspace` (custom) | airspace occupancy counts; `bles`/`trackers` present only when BLE is active |
| `done` | `of=<cmd>` `n=<count>` | any listing cmd | end-of-listing marker so CC knows the batch is complete |

### `alert` kind-specific fields (the detector that fires it in parentheses)
- `deauth` (`defend`): `bssid`(mac, busiest source) `count`(total deauth+disassoc) `deauth` `disassoc`. Fired only when count > 0.
- `eviltwin` (`eviltwin`): `ssid`(hex) `bssids`(count advertising this ESSID) `open`(open BSSIDs) `enc`(encrypted BSSIDs). One per flagged ESSID.
- `weak` (`apaudit`): `bssid`(mac) `ssid`(hex) `grade`(0=open 1=WEP 2=legacy-WPA) `wps`(=1 if also WPS). One per weak-encryption AP.
- `wps` (`apaudit`): `bssid`(mac) `ssid`(hex) `grade`(3=WPA2 4=WPA3 …) `wps`(=1). One per WPS-advertising AP that is otherwise adequately encrypted.
- `tracker` (`btracker`): `addr`(mac) `vendor` (AirTag/Tile/SmartTag/Chipolo/PebbleBee/GoogleFMN)
- `bleflood` (`bleflood`): `rate` (adv/s) `vendor`
- `blehid` (`blehid`): `addr`(mac) `name`(hex) — a BLE HID (keyboard/mouse) device, an injection surface
- `watch` (`watch scan`, custom): `mac`(mac, the watched target) `rssi`(dBm) `band=wifi\|ble` — a watchlisted BSSID/BLE-addr is present on this sweep. One per hit.

## Escaping / encoding rules (for the CC parser)
- `mac` — lowercase `aa:bb:cc:dd:ee:ff`.
- `hex` — lowercase, even-length; decode to bytes; an empty value (`ssid=`) is a zero-length field.
- integers — decimal, may be negative (`rssi`).
- enums — fixed lowercase tokens.

## Versioning
The tag is `LXVEOS/1`. Additive changes (new `type`, new key) do not bump the version. A breaking change to an
existing field's meaning would bump to `LXVEOS/2`; CC checks the version and refuses to misparse a newer major.
