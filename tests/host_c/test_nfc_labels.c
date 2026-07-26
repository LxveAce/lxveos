// Host-side unit test for lxveos_nfc_card_type (pure ISO-14443A SAK -> card-type label). libc + the esp_err
// stub that lxveos_nfc.h pulls in — no PN532/I2C driver. Built + run by tests/host_c/run.sh. Aborts (non-zero)
// on first failure. Values are cross-checked against NXP AN10833 / the proxmark3 hf14a type table.
#include "lxveos_nfc.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    // Exact catalogued SAK values (NXP AN10833 / proxmark3 hf14a).
    assert(strcmp(lxveos_nfc_card_type(0x00), "MIFARE Ultralight/NTAG") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x01), "TNP3xxx") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x08), "MIFARE Classic 1K") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x09), "MIFARE Mini 0.3K") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x10), "MIFARE Plus 2K (SL2)") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x11), "MIFARE Plus 4K (SL2)") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x18), "MIFARE Classic 4K") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x19), "MIFARE Classic 2K") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x20), "ISO14443-4 (DESFire/Plus)") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x28), "SmartMX + Classic 1K") == 0);
    assert(strcmp(lxveos_nfc_card_type(0x38), "SmartMX + Classic 4K") == 0);

    // Bit-field fallback for uncatalogued SAKs (ISO-14443-3 / AN10833), first-match order:
    // 0x04 set -> cascade/partial UID (the first cascade level of a 7-byte UID reports SAK 0x04).
    assert(strcmp(lxveos_nfc_card_type(0x04), "cascade (partial UID)") == 0);
    // 0x20 set (ISO14443-4) with no exact match, e.g. 0x60.
    assert(strcmp(lxveos_nfc_card_type(0x60), "ISO14443-4 card") == 0);
    // 0x08 set (Classic-compatible) with no exact match, e.g. 0x88.
    assert(strcmp(lxveos_nfc_card_type(0x88), "MIFARE Classic-compatible") == 0);
    // No recognised bits -> honest unknown (not a fabricated type).
    assert(strcmp(lxveos_nfc_card_type(0x02), "ISO14443A (unknown SAK)") == 0);

    // Total over the byte domain: never NULL for any SAK.
    for (int s = 0; s < 256; s++) {
        assert(lxveos_nfc_card_type((uint8_t)s) != NULL);
    }

    printf("test_nfc_labels: all passed\n");
    return 0;
}
