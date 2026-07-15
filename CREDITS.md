# Credits

LxveOS builds on the great open ESP32 security-firmware community — with proper credit and **without
competing**. Ideas/concepts are free to learn from (not copyrightable); only permissively-licensed **code**
is reused directly. The full per-project map is in [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md).

## Direct code reuse (MIT — vendored with notice)
- **ESP32 Marauder** — Just Call Me Koko (MIT), *Copyright (c) 2020 Just Call Me Koko* — Wi-Fi scan/monitor,
  wardrive WiGLE CSV, Evil Portal, serial protocol, and the passive Pwnagotchi-presence detector ported from its
  "Detect Pwnagotchi" menu. Full MIT license text retained in `THIRD-PARTY-LICENSES.md`.
- **ESP32-DIV** — CiferTech (MIT) — recon/scan/GPS/SD paths (jammer code excluded, by policy).
- **risinek/esp32-wifi-penetration-tool** (MIT) — ESP-IDF PMKID / EAPOL-handshake capture approach (LxveOS emits
  hashcat-22000 `WPA*01`/`WPA*02` lines directly; PCAP/HCCAPX export is not shipped).
- **Evil-M5Project** (MIT) — master/slave swarm coordination + detectors.
- **WiFi Nugget** — Alex Lynd / HakCat (MIT) — HAL + onboarding UX ideas.

## Ideas only, clean-room (copyleft — no code copied into this MIT core)
- **GhostESP** (GPL-3.0), **Bruce** (AGPL-3.0), **M5Stick-NEMO** (GPL), **minigotchi** (GPL) — UX, detectors,
  scripting, pet-mode concepts reimplemented independently. **Spacehuhn deauther** (CC BY-NC) — ethos only.

## Stance
- **No endorsement / trademarks:** "Marauder", "GhostESP", "Bruce", "NEMO" etc. are used only for factual
  compatibility identification, never as the LxveOS product name; no affiliation implied.
- **Clean-room clears copyright, not patents** (e.g. MIFARE Crypto1, FIDO2) — documented where relevant.
- **No jammer, ever.** LxveOS authors no RF-jamming / deauth-flood / DoS transmit frames; Wi-Fi deauth ships as
  detection only. The labelled offensive ops (evil-portal, BLE HID, sub-GHz replay, nRF24 MouseJack, NFC clone) are
  retained but compiled behind a two-factor `arm` gate, for owned/authorized hardware only. See `RESPONSIBLE-USE.md`.
- Not legal advice; a lawyer reviews before any commercial or copyleft (GPL/AGPL) distribution.
