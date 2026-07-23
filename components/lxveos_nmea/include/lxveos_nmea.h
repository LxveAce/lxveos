#pragma once
// lxveos_nmea — dependency-free (libc-only) NMEA-0183 GPS sentence parser: checksum verification plus the
// GxRMC / GxGGA position sentences (latitude/longitude in decimal degrees, fix quality, UTC time). Kept a
// standalone component so it unit-tests on the host (tests/host_c/test_nmea.c) with real sentence fixtures.
// The GPS wardrive-log op (wardrive_log) stays implemented:false until a GPS module is wired to hardware;
// this is only the pure parser core, verified in CI.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One parsed position fix.
typedef struct {
    bool    valid;        // the fix is usable: RMC status 'A', or GGA fix_quality > 0
    double  lat;          // decimal degrees, North positive / South negative (0 if the field was empty)
    double  lon;          // decimal degrees, East positive / West negative (0 if the field was empty)
    int     fix_quality;  // GGA fix indicator (0 none, 1 GPS, 2 DGPS, ...); RMC maps status A->1, V->0
    bool    has_time;     // a valid hhmmss UTC time was present
    uint8_t hour, minute, second;  // UTC time of the fix (00:00:00 if !has_time)
} lxveos_nmea_fix_t;

// Verify a NMEA sentence's checksum: the XOR of every byte between '$' and '*' must equal the two hex digits
// after '*'. `s` may start at '$' or the first content byte; `len` is its length. Returns false for a NULL or
// too-short input, a sentence with no '*CS', or a non-hex checksum. Pure, host-tested.
bool lxveos_nmea_checksum_ok(const char *s, size_t len);

// Parse one GxRMC or GxGGA sentence (any talker id: GP / GN / GL / ...) into *out (always zeroed first).
// Verifies the checksum, then pulls out latitude/longitude (decimal degrees), fix quality/validity, and the
// UTC time. Returns true for a recognised, checksum-valid RMC or GGA (out->valid then says whether the FIX is
// usable); false for a bad checksum, an unrecognised sentence, or too few fields.
bool lxveos_nmea_parse(const char *s, size_t len, lxveos_nmea_fix_t *out);

#ifdef __cplusplus
}
#endif
