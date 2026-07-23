#pragma once
// lxveos_wifi_eapol — the PURE hashcat-22000 line formatter for the EAPOL/PMKID capture. It builds the
// ready-to-crack `WPA*01*` (PMKID) and `WPA*02*` (EAPOL/MIC) lines from already-parsed handshake fields.
// libc-only (no ESP-IDF), so it host-tests off-target (tests/host_c/test_wifi_eapol.c); the promiscuous
// capture in lxveos_wifi.c parses the frames and calls these to emit each artifact. A format bug here would
// silently produce uncrackable output, which is exactly why the formatting is split out and tested.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a hashcat-22000 WPA*01 (PMKID) line into out[cap]:
//   WPA*01*<pmkid>*<ap-mac>*<sta-mac>*<essid-hex>***
// `pmkid` is 16 bytes, `ap`/`sta` are 6-byte MACs, `essid` is the AP's NUL-terminated SSID (hex-encoded — it
// is the PBKDF2 salt). The trailing `***` are the three empty 22000 fields (anonce/eapol/messagepair) a PMKID
// line has none of. Returns the line length (excluding the NUL), or 0 if any pointer is NULL, cap is 0, or the
// ESSID is empty (an empty-ESSID PMKID is uncrackable, so the capture drops it). `out` is always NUL-terminated.
size_t lxveos_hc22000_pmkid(char *out, size_t cap, const uint8_t pmkid[16], const uint8_t ap[6],
                            const uint8_t sta[6], const char *essid);

// Build a hashcat-22000 WPA*02 (EAPOL/MIC) line into out[cap]:
//   WPA*02*<mic>*<ap-mac>*<sta-mac>*<essid-hex>*<anonce>*<eapol>*<messagepair>
// `mic` is 16 bytes, `ap`/`sta` 6-byte MACs, `anonce` 32 bytes, `eapol` the `eapol_len`-byte EAPOL frame (with
// its MIC zeroed by the caller), `messagepair` the hcxtools pair code ("00"/"02"/"05"). Returns the line length,
// or 0 on a NULL pointer, cap 0, or an empty ESSID (uncrackable). `out` is always NUL-terminated.
size_t lxveos_hc22000_eapol(char *out, size_t cap, const uint8_t mic[16], const uint8_t ap[6],
                            const uint8_t sta[6], const char *essid, const uint8_t anonce[32],
                            const uint8_t *eapol, size_t eapol_len, const char *messagepair);

#ifdef __cplusplus
}
#endif
