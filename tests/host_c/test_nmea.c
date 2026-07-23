// Host-side unit test for lxveos_nmea (NMEA-0183 checksum + GxRMC/GxGGA parse). Pure libc, no ESP-IDF. Built
// + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion. The position fixtures
// are the canonical NMEA reference sentences (their *47 / *6A checksums are well documented).
#include "lxveos_nmea.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_checksum(void)
{
    assert(lxveos_nmea_checksum_ok("$A*41", 5));    // XOR("A") = 0x41
    assert(!lxveos_nmea_checksum_ok("$A*42", 5));   // wrong checksum value
    assert(lxveos_nmea_checksum_ok("$AB*03", 6));   // 0x41 ^ 0x42 = 0x03
    assert(lxveos_nmea_checksum_ok("A*41", 4));     // a leading '$' is optional
    assert(!lxveos_nmea_checksum_ok("$AB", 3));     // no '*'
    assert(!lxveos_nmea_checksum_ok("$A*4", 4));    // only one hex digit
    assert(!lxveos_nmea_checksum_ok("$A*4G", 5));   // a non-hex checksum digit
    assert(!lxveos_nmea_checksum_ok(NULL, 5));
    assert(!lxveos_nmea_checksum_ok("$A", 2));      // too short
}

static void test_gga(void)
{
    const char *s = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    lxveos_nmea_fix_t fix;
    assert(lxveos_nmea_parse(s, strlen(s), &fix));
    assert(fix.fix_quality == 1 && fix.valid);
    assert(fix.lat > 48.1172 && fix.lat < 48.1174);   // 48 deg + 07.038'/60 = 48.1173
    assert(fix.lon > 11.5166 && fix.lon < 11.5168);   // 011 deg + 31.000'/60 = 11.5167
    assert(fix.has_time && fix.hour == 12 && fix.minute == 35 && fix.second == 19);
}

static void test_rmc(void)
{
    const char *s = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    lxveos_nmea_fix_t fix;
    assert(lxveos_nmea_parse(s, strlen(s), &fix));
    assert(fix.valid && fix.fix_quality == 1);
    assert(fix.lat > 48.1172 && fix.lat < 48.1174);
    assert(fix.lon > 11.5166 && fix.lon < 11.5168);
    assert(fix.has_time && fix.hour == 12 && fix.minute == 35 && fix.second == 19);
}

static void test_rejects(void)
{
    lxveos_nmea_fix_t fix;
    // a flipped checksum on an otherwise-valid GGA is rejected (last digit 7 -> 8)
    const char *bad = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48";
    assert(!lxveos_nmea_parse(bad, strlen(bad), &fix));
    // a checksum-valid but unrecognised sentence type is rejected (XOR("XXX") = 0x58)
    assert(!lxveos_nmea_parse("$XXX*58", 7, &fix));
    // NULL / too-short inputs
    assert(!lxveos_nmea_parse(NULL, 10, &fix));
    assert(!lxveos_nmea_parse("$", 1, &fix));
}

int main(void)
{
    test_checksum();
    test_gga();
    test_rmc();
    test_rejects();
    printf("test_nmea: all tests passed\n");
    return 0;
}
