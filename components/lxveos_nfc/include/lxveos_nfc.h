#pragma once
// LxveOS NFC (PN532) — the `nfc_read` recon op (increment 1: reader identify + read a 13.56 MHz card's UID).
// The NXP PN532 is the ubiquitous 13.56 MHz NFC/RFID front-end (Flipper, ACR122, Elechouse module). This
// increment talks to it over I2C, runs SAMConfiguration + GetFirmwareVersion to identify the reader, then
// InListPassiveTarget to read the UID of one ISO-14443A card in the field (Mifare Classic/Ultralight, most
// access badges). RECEIVE/READ ONLY — it does not write or emulate. Clone/emulate (`nfc_clone`/`nfc_emulate`)
// is a later increment.
//
// Pins are operator-supplied (add-on module): I2C SDA + SCL. Not cap-gated.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up I2C on the given pins and identify the PN532 (SAMConfiguration + GetFirmwareVersion). Stores the
// bus/device handles. Returns ESP_OK if the reader answered (see lxveos_nfc_version()), ESP_ERR_TIMEOUT /
// esp_err_t if it did not, or ESP_ERR_INVALID_STATE if already begun.
esp_err_t lxveos_nfc_begin(int sda, int scl);

// Release the I2C bus/device (safe when not begun).
esp_err_t lxveos_nfc_end(void);

// The PN532 firmware version byte read at begin() (0 if none answered), and the IC revision.
uint8_t lxveos_nfc_version(void);
uint8_t lxveos_nfc_ic(void);
// True if a PN532 responded at begin().
bool lxveos_nfc_present(void);

// Poll once (up to `timeout_ms`) for one ISO-14443A card and copy its UID into uid[] (up to `uid_cap`
// bytes), setting *uid_len. Also reports SAK/ATQA via *sak/*atqa (may be NULL). Returns ESP_OK on a read,
// ESP_ERR_TIMEOUT if no card entered the field, ESP_ERR_INVALID_STATE if not begun, or an esp_err_t.
esp_err_t lxveos_nfc_read_uid(uint32_t timeout_ms, uint8_t *uid, size_t uid_cap, size_t *uid_len,
                              uint8_t *sak, uint16_t *atqa);

// ── Increment 2: UID clone (the nfc_clone op) ─────────────────────────────────────────────────────────
// Write a 4-byte `uid` into block 0 of a presented writable/"magic" Mifare Classic card (the common access-
// badge clone). Selects the card, authenticates block 0 with the default key A (FF FF FF FF FF FF), and
// writes block 0 = UID(4) + BCC + SAK 0x08 + ATQA 0x0004 + manufacturer bytes. Returns ESP_OK on a write,
// ESP_ERR_TIMEOUT if no card was presented, ESP_ERR_INVALID_STATE if not begun, ESP_ERR_INVALID_ARG for a
// non-4-byte UID, or ESP_FAIL if auth/write was refused (not a magic card / wrong key).
//
// HW + magic-card-type pending: this is the standard Gen2/CUID direct-write sequence; Gen1a cards need the
// backdoor unlock commands (a further increment). Structurally correct + CI-green; extra validates on HW.
esp_err_t lxveos_nfc_clone_write(const uint8_t *uid, size_t uid_len);

#ifdef __cplusplus
}
#endif
