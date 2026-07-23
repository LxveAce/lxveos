// lxveos_nmea — see lxveos_nmea.h. Dependency-free NMEA-0183 parser (checksum + GxRMC/GxGGA), host-tested
// off-target (tests/host_c/test_nmea.c). libc-only: string.h + strtod, no math.h, no allocation.
#include "lxveos_nmea.h"

#include <stdlib.h>  // strtod
#include <string.h>  // memset / memcpy

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool lxveos_nmea_checksum_ok(const char *s, size_t len)
{
    if (s == NULL || len < 4) {
        return false;
    }
    size_t i = (s[0] == '$') ? 1u : 0u;
    uint8_t cs = 0;
    size_t star = len;  // sentinel: no '*' seen
    for (; i < len; i++) {
        if (s[i] == '*') {
            star = i;
            break;
        }
        if (s[i] == '\r' || s[i] == '\n') {
            break;
        }
        cs ^= (uint8_t)s[i];
    }
    if (star == len || star + 2 >= len) {  // need '*' and two hex digits after it
        return false;
    }
    int hi = hex_nibble(s[star + 1]);
    int lo = hex_nibble(s[star + 2]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    return cs == (uint8_t)((hi << 4) | lo);
}

// Parse a decimal integer from a bounded field (digits only, stops at the first non-digit). Empty -> 0.
static int field_int(const char *f, size_t n)
{
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (f[i] < '0' || f[i] > '9') {
            break;
        }
        v = v * 10 + (f[i] - '0');
    }
    return v;
}

// Parse an "hhmmss(.sss)" UTC field into out->hour/minute/second; sets has_time only on a full hhmmss.
static void parse_time(const char *f, size_t n, lxveos_nmea_fix_t *out)
{
    if (n < 6) {
        return;
    }
    for (int k = 0; k < 6; k++) {
        if (f[k] < '0' || f[k] > '9') {
            return;
        }
    }
    out->hour = (uint8_t)((f[0] - '0') * 10 + (f[1] - '0'));
    out->minute = (uint8_t)((f[2] - '0') * 10 + (f[3] - '0'));
    out->second = (uint8_t)((f[4] - '0') * 10 + (f[5] - '0'));
    out->has_time = true;
}

// Convert a "ddmm.mmmm" (or "dddmm.mmmm") field + a hemisphere char into decimal degrees. Leaves *out
// untouched (the caller pre-zeroes) if the field is empty or too long to buffer.
static void parse_latlon(const char *f, size_t n, char hemi, double *out)
{
    if (n == 0 || n >= 16) {
        return;
    }
    char tmp[16];
    memcpy(tmp, f, n);
    tmp[n] = '\0';
    char *end = NULL;
    double v = strtod(tmp, &end);
    if (end == tmp) {
        return;  // no numeric content
    }
    int deg = (int)(v / 100.0);                    // whole degrees are the leading dd / ddd
    double minutes = v - (double)deg * 100.0;      // the trailing mm.mmmm are minutes
    double dec = (double)deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') {
        dec = -dec;
    }
    *out = dec;
}

bool lxveos_nmea_parse(const char *s, size_t len, lxveos_nmea_fix_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (s == NULL || len < 6 || !lxveos_nmea_checksum_ok(s, len)) {
        return false;
    }
    size_t p = (s[0] == '$') ? 1u : 0u;
    if (p + 5 > len) {
        return false;
    }
    const char *typ = s + p + 2;  // 2-char talker id, then the 3-char sentence type
    bool is_rmc = (typ[0] == 'R' && typ[1] == 'M' && typ[2] == 'C');
    bool is_gga = (typ[0] == 'G' && typ[1] == 'G' && typ[2] == 'A');
    if (!is_rmc && !is_gga) {
        return false;
    }

    // Split into comma-separated fields, stopping at '*' / CR / LF. fs[0] is the "$xxTYP" token.
    const char *fs[20];
    size_t fl[20];
    int nf = 1;
    fs[0] = s + p;
    fl[0] = 0;
    for (size_t i = p; i < len; i++) {
        char c = s[i];
        if (c == '*' || c == '\r' || c == '\n') {
            break;
        }
        if (c == ',') {
            if (nf >= 20) {
                break;
            }
            fs[nf] = s + i + 1;
            fl[nf] = 0;
            nf++;
        } else {
            fl[nf - 1]++;
        }
    }
    if (nf < 7) {  // both RMC and GGA carry position by field index 6 at the latest
        return false;
    }

    int t_idx, lat_idx, lon_idx;
    if (is_rmc) {
        t_idx = 1;
        lat_idx = 3;
        lon_idx = 5;
        out->valid = (fl[2] >= 1 && fs[2][0] == 'A');
        out->fix_quality = out->valid ? 1 : 0;
    } else {  // GGA
        t_idx = 1;
        lat_idx = 2;
        lon_idx = 4;
        out->fix_quality = field_int(fs[6], fl[6]);
        out->valid = (out->fix_quality > 0);
    }

    parse_time(fs[t_idx], fl[t_idx], out);
    if (fl[lat_idx] > 0 && fl[lat_idx + 1] >= 1) {
        parse_latlon(fs[lat_idx], fl[lat_idx], fs[lat_idx + 1][0], &out->lat);
    }
    if (fl[lon_idx] > 0 && fl[lon_idx + 1] >= 1) {
        parse_latlon(fs[lon_idx], fl[lon_idx], fs[lon_idx + 1][0], &out->lon);
    }
    return true;
}
