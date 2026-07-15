// lxveos_radiomath — see lxveos_radiomath.h. Pure math for the external-radio drivers; no ESP-IDF, no
// hardware. Kept byte-for-byte identical to the arithmetic the drivers used inline before extraction, so
// this is a refactor (dedup + host-testable), not a behaviour change.
#include "lxveos_radiomath.h"

#define CC1101_XOSC_HZ 26000000.0f   // 26 MHz crystal (standard on CC1101 modules)

uint32_t lxveos_cc1101_freq_to_word(float mhz)
{
    return (uint32_t)((mhz * 1000000.0f) * 65536.0f / CC1101_XOSC_HZ);
}

int8_t lxveos_cc1101_rssi_to_dbm(uint8_t raw)
{
    int rssi_dec = (raw >= 128) ? (raw - 256) : raw;
    int dbm = rssi_dec / 2 - 74;
    if (dbm < -128) {
        dbm = -128;
    }
    if (dbm > 127) {
        dbm = 127;
    }
    return (int8_t)dbm;
}

uint8_t lxveos_pn532_lcs(uint8_t len)
{
    return (uint8_t)(~len + 1);
}

uint8_t lxveos_pn532_dcs(uint8_t tfi, const uint8_t *payload, uint8_t plen)
{
    uint8_t sum = tfi;
    for (uint8_t k = 0; k < plen; k++) {
        sum = (uint8_t)(sum + payload[k]);
    }
    return (uint8_t)(~sum + 1);
}

size_t lxveos_pn532_build_frame(uint8_t tfi, const uint8_t *cmd, uint8_t clen, uint8_t *out, size_t out_cap)
{
    // 00 00 FF LEN LCS TFI cmd[0..clen-1] DCS 00  => clen + 8 bytes total.
    size_t need = (size_t)clen + 8u;
    if (out == NULL || (cmd == NULL && clen > 0) || out_cap < need) {
        return 0;
    }
    uint8_t len = (uint8_t)(clen + 1);   // + TFI
    size_t i = 0;
    out[i++] = 0x00;
    out[i++] = 0x00;
    out[i++] = 0xFF;
    out[i++] = len;
    out[i++] = lxveos_pn532_lcs(len);
    out[i++] = tfi;
    uint8_t sum = tfi;
    for (uint8_t k = 0; k < clen; k++) {
        out[i++] = cmd[k];
        sum = (uint8_t)(sum + cmd[k]);
    }
    out[i++] = (uint8_t)(~sum + 1);      // DCS
    out[i++] = 0x00;                     // postamble
    return i;
}

bool lxveos_pn532_frame_valid(const uint8_t *frame, size_t n)
{
    // Minimum frame is an empty payload: 00 00 FF 01 FF D4 DCS 00 => len byte 1, total 8 bytes.
    if (frame == NULL || n < 8u) {
        return false;
    }
    if (frame[0] != 0x00 || frame[1] != 0x00 || frame[2] != 0xFF) {
        return false;
    }
    uint8_t len = frame[3];
    if (len == 0 || (uint8_t)(len + frame[4]) != 0) {   // LCS: LEN + LCS == 0 mod 256
        return false;
    }
    if ((size_t)len + 7u > n) {                         // TFI..DCS + postamble must be in-bounds
        return false;
    }
    uint8_t sum = 0;
    for (uint8_t k = 0; k < len; k++) {
        sum = (uint8_t)(sum + frame[5 + k]);            // TFI + payload
    }
    uint8_t dcs = frame[5 + (size_t)len];
    if ((uint8_t)(sum + dcs) != 0) {                    // DCS: (TFI+payload) + DCS == 0 mod 256
        return false;
    }
    return frame[6 + (size_t)len] == 0x00;              // postamble
}

uint8_t lxveos_mifare_bcc4(const uint8_t uid[4])
{
    return (uint8_t)(uid[0] ^ uid[1] ^ uid[2] ^ uid[3]);
}

uint8_t lxveos_unifying_checksum(const uint8_t *frame, size_t n)
{
    if (frame == NULL || n == 0) {
        return 0;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < n; i++) {   // first n-1 bytes; the last byte is the checksum slot
        sum = (uint8_t)(sum + frame[i]);
    }
    return (uint8_t)(0 - sum);
}

bool lxveos_unifying_checksum_ok(const uint8_t *frame, size_t n)
{
    if (frame == NULL || n == 0) {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum = (uint8_t)(sum + frame[i]);
    }
    return sum == 0;
}

// Pulse classes relative to the base time Te (see lxveos_ook_decode). A short pulse is ~1 Te, a long one
// ~3 Te; anything past ~6 Te is an inter-frame sync gap, not part of a bit.
#define OOK_NOISE_FLOOR_US 50   // ignore sub-50us glitches when estimating Te
enum { OOK_SHORT = 0, OOK_LONG = 1, OOK_GAP = 2 };

static int ook_classify(uint16_t pulse, uint16_t te)
{
    if (pulse < (uint16_t)(2 * te)) {
        return OOK_SHORT;
    }
    if (pulse < (uint16_t)(6 * te)) {
        return OOK_LONG;
    }
    return OOK_GAP;
}

size_t lxveos_ook_decode(const uint16_t *durations, size_t n, char *bits, size_t bits_cap)
{
    if (durations == NULL || bits == NULL || bits_cap == 0 || n < 2) {
        return 0;
    }
    // Te = shortest pulse above the noise floor. Te*6 must stay in uint16 range for the classifier.
    uint16_t te = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t d = durations[i];
        if (d >= OOK_NOISE_FLOOR_US && (te == 0 || d < te)) {
            te = d;
        }
    }
    if (te == 0 || (uint32_t)te * 6u > 0xFFFFu) {
        return 0;
    }
    // EV1527/PT2262 lead with a short HIGH pilot then a long LOW sync gap; if durations[1] is that gap, the
    // data bits begin at index 2. Otherwise assume the capture starts on data. (Only the leading LOW is
    // treated as sync — a gap later in the train is a frame boundary that ends decoding, not a skip point.)
    size_t start = (ook_classify(durations[1], te) == OOK_GAP) ? 2 : 0;
    // Decode HIGH/LOW pairs: short-high+long-low => '0', long-high+short-low => '1'. Stop at a gap / an
    // ambiguous pair / the end.
    size_t nbits = 0;
    for (size_t i = start; i + 1 < n && nbits < bits_cap; i += 2) {
        int hi = ook_classify(durations[i], te);
        int lo = ook_classify(durations[i + 1], te);
        if (lo == OOK_GAP) {
            break;                      // end of this frame
        }
        if (hi == OOK_SHORT && lo == OOK_LONG) {
            bits[nbits++] = '0';
        } else if (hi == OOK_LONG && lo == OOK_SHORT) {
            bits[nbits++] = '1';
        } else {
            break;                      // neither PWM symbol — stop rather than emit noise
        }
    }
    return nbits;
}
