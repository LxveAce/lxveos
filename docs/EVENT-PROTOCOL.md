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
| `sta` | `mac`(mac) `ap`(mac) `rssi` | `stations` | client station; `ap` = associated BSSID if known |
| `probe` | `mac`(mac) `ssid`(hex) `rssi` | `probes` | probe-request; `ssid` = requested SSID |
| `ble` | `addr`(mac) `name`(hex) `rssi` `appearance` `svc`(hex) | `blescan` | fields present when known |
| `hs` | `kind=pmkid\|eapol` `bssid`(mac) `sta`(mac) `essid`(hex) | `capture` | one per crackable capture; feeds Crack Lab |
| `pcap` | `id` `bytes` | `pcap_log` | a pcap segment was written (needs storage; HW) |
| `arm` | `state=safe\|pending\|armed` `token`(pending only) `window`(s, pending) `idle`(s, armed) | `arm`/`disarm`/timeout | arm state change |
| `alert` | `kind=deauth\|eviltwin\|tracker\|bleflood\|blehid\|wps` `...`(kind-specific) | `defend`/`eviltwin`/`apaudit`/`bleflood`/`btracker`/`blehid` | a detector fired |
| `snapshot` | `aps` `stas` `bles` `alerts` | `recon` (custom) | airspace summary counts |
| `done` | `of=<cmd>` `n=<count>` | any listing cmd | end-of-listing marker so CC knows the batch is complete |

### `alert` kind-specific fields
- `deauth`: `bssid`(mac) `count`
- `eviltwin`: `ssid`(hex) `bssids`(count of duplicate BSSIDs)
- `tracker`: `addr`(mac) `vendor` (AirTag/Tile/SmartTag/Chipolo/PebbleBee/GoogleFMN)
- `bleflood`: `rate` (adv/s) `vendor`
- `wps`: `bssid`(mac) `ssid`(hex)

## Escaping / encoding rules (for the CC parser)
- `mac` — lowercase `aa:bb:cc:dd:ee:ff`.
- `hex` — lowercase, even-length; decode to bytes; an empty value (`ssid=`) is a zero-length field.
- integers — decimal, may be negative (`rssi`).
- enums — fixed lowercase tokens.

## Versioning
The tag is `LXVEOS/1`. Additive changes (new `type`, new key) do not bump the version. A breaking change to an
existing field's meaning would bump to `LXVEOS/2`; CC checks the version and refuses to misparse a newer major.
