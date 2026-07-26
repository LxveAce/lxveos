// lxveos_nfc_labels — pure ISO-14443A card-type label from the SAK byte, split out of lxveos_nfc.c so it can
// be host-unit-tested (tests/host_c/test_nfc_labels.c) with no PN532/I2C driver. The mapping follows NXP
// AN10833 "MIFARE type identification procedure" and the proxmark3 hf14a type table: SAK — not ATQA — is the
// authoritative card-type discriminator (NXP states ATQA is not a reliable type indicator), so the nfc_read op
// prints ATQA raw and derives the type from SAK here. RX/identify only; nothing here writes or emulates a card.
#include "lxveos_nfc.h"

#include <stdint.h>

const char *lxveos_nfc_card_type(uint8_t sak)
{
    // Exact SAK values for the common 13.56 MHz products (NXP AN10833 / proxmark3 hf14a type table).
    switch (sak) {
    case 0x00: return "MIFARE Ultralight/NTAG";
    case 0x01: return "TNP3xxx";
    case 0x08: return "MIFARE Classic 1K";
    case 0x09: return "MIFARE Mini 0.3K";
    case 0x10: return "MIFARE Plus 2K (SL2)";
    case 0x11: return "MIFARE Plus 4K (SL2)";
    case 0x18: return "MIFARE Classic 4K";
    case 0x19: return "MIFARE Classic 2K";
    case 0x20: return "ISO14443-4 (DESFire/Plus)";
    case 0x28: return "SmartMX + Classic 1K";
    case 0x38: return "SmartMX + Classic 4K";
    default: break;
    }
    // Uncatalogued SAK: fall back to the ISO-14443-3 SAK bit fields (AN10833) —
    //   b2 0x04 = UID not complete (cascade in progress), b5 0x20 = ISO/IEC 14443-4 (T=CL/RATS),
    //   b3 0x08 = MIFARE Classic protocol compatible. Tested first-match in that order.
    if (sak & 0x04u) {
        return "cascade (partial UID)";
    }
    if (sak & 0x20u) {
        return "ISO14443-4 card";
    }
    if (sak & 0x08u) {
        return "MIFARE Classic-compatible";
    }
    return "ISO14443A (unknown SAK)";
}
