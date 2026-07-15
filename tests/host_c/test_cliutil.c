// Host-side unit test for lxveos_cliutil (parse_int_arg + sanitize_copy). Pure libc, no ESP-IDF toolchain.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_cliutil.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_parse_mac(void)
{
    uint8_t m[6];
    // canonical lower-case
    assert(parse_mac("de:ad:be:ef:00:01", m));
    assert(m[0] == 0xde && m[1] == 0xad && m[2] == 0xbe && m[3] == 0xef && m[4] == 0x00 && m[5] == 0x01);
    // upper-case and mixed-case parse identically
    assert(parse_mac("DE:AD:BE:EF:00:01", m) && m[0] == 0xde && m[5] == 0x01);
    assert(parse_mac("De:Ad:bE:eF:0a:0B", m) && m[2] == 0xbe && m[4] == 0x0a && m[5] == 0x0b);
    // all-zero and all-ff bounds
    assert(parse_mac("00:00:00:00:00:00", m) && m[0] == 0x00 && m[5] == 0x00);
    assert(parse_mac("ff:ff:ff:ff:ff:ff", m) && m[0] == 0xff && m[5] == 0xff);
    // rejects: too few octets, too many, short octet, wrong separator, non-hex, trailing junk, empty
    assert(!parse_mac("de:ad:be:ef:00", m));         // 5 octets
    assert(!parse_mac("de:ad:be:ef:00:01:02", m));   // 7 octets
    assert(!parse_mac("d:ad:be:ef:00:01", m));       // single-digit octet
    assert(!parse_mac("de-ad-be-ef-00-01", m));      // dash separator
    assert(!parse_mac("de:ad:be:ef:00:0g", m));      // non-hex nibble
    assert(!parse_mac("de:ad:be:ef:00:01 ", m));     // trailing space
    assert(!parse_mac("de:ad:be:ef:00:01x", m));     // trailing char
    assert(!parse_mac("de:ad:be:ef:00:", m));        // trailing colon, no sixth octet
    assert(!parse_mac("", m));                        // empty
    assert(!parse_mac("deadbeef0001", m));           // no separators
    // NULL args -> false, never dereferenced (must not read one past a boundary either)
    assert(!parse_mac(NULL, m));
    assert(!parse_mac("de:ad:be:ef:00:01", NULL));
}

static void test_parse_hex_octets(void)
{
    uint8_t b[6];
    // canonical 4-byte NFC UID, upper- and lower-case parse identically
    assert(parse_hex_octets("DEADBEEF", b, 4));
    assert(b[0] == 0xde && b[1] == 0xad && b[2] == 0xbe && b[3] == 0xef);
    assert(parse_hex_octets("deadbeef", b, 4) && b[0] == 0xde && b[3] == 0xef);
    // boundary byte values
    assert(parse_hex_octets("00ff10ab", b, 4) && b[0] == 0x00 && b[1] == 0xff && b[2] == 0x10 && b[3] == 0xab);
    // works for other widths (1 byte, 6 bytes = a colon-less MAC)
    assert(parse_hex_octets("0f", b, 1) && b[0] == 0x0f);
    assert(parse_hex_octets("001122334455", b, 6) && b[0] == 0x00 && b[5] == 0x55);
    // rejects: too short, too long (trailing), odd length, non-hex, empty
    assert(!parse_hex_octets("deadbe", b, 4));      // 6 chars, 4 bytes wanted
    assert(!parse_hex_octets("deadbeef00", b, 4));  // 10 chars -> trailing rejected
    assert(!parse_hex_octets("deadbee", b, 4));     // odd length (7)
    assert(!parse_hex_octets("deadbeeg", b, 4));    // non-hex nibble
    assert(!parse_hex_octets("", b, 4));            // empty
    assert(!parse_hex_octets("0", b, 1));           // one nibble, one byte wanted
    // NULL args -> false, never dereferenced
    assert(!parse_hex_octets(NULL, b, 4));
    assert(!parse_hex_octets("dead", NULL, 2));
}

static void test_parse_int_arg(void)
{
    long v = -999;
    // in range (inclusive endpoints)
    assert(parse_int_arg("6", 1, 13, &v) && v == 6);
    assert(parse_int_arg("1", 1, 13, &v) && v == 1);
    assert(parse_int_arg("13", 1, 13, &v) && v == 13);
    // negative range
    assert(parse_int_arg("-42", -100, 0, &v) && v == -42);
    // out of range -> false, *out left untouched
    v = 7;
    assert(!parse_int_arg("0", 1, 13, &v) && v == 7);
    assert(!parse_int_arg("14", 1, 13, &v) && v == 7);
    // non-numeric / empty -> false
    assert(!parse_int_arg("x", 1, 13, &v));
    assert(!parse_int_arg("", 1, 13, &v));
    // trailing garbage: digits-then-stop convention ("8x" -> 8)
    assert(parse_int_arg("8x", 1, 13, &v) && v == 8);
    // NULL args -> false, never dereferenced
    assert(!parse_int_arg(NULL, 1, 13, &v));
    assert(!parse_int_arg("5", 1, 13, NULL));
}

static void test_sanitize_copy(void)
{
    char buf[16];
    // plain printable text passes through unchanged
    sanitize_copy(buf, sizeof(buf), "MyNet");
    assert(strcmp(buf, "MyNet") == 0);
    // ESC (0x1b) and DEL (0x7f) -> '.', printable bytes kept. The "\x7f" is split from the next 'c' so the
    // hex escape can't swallow it (c is a hex digit).
    sanitize_copy(buf, sizeof(buf), "a\x1b[2Jb\x7f" "c");
    assert(strcmp(buf, "a.[2Jb.c") == 0);
    // embedded newline/tab (both < 0x20) -> '.'
    sanitize_copy(buf, sizeof(buf), "x\n\ty");
    assert(strcmp(buf, "x..y") == 0);
    // truncation at cap-1, always NUL-terminated (canary at index 3 must be overwritten with '\0')
    char small[4];
    small[3] = 'Z';
    sanitize_copy(small, sizeof(small), "abcdef");
    assert(strcmp(small, "abc") == 0 && small[3] == '\0');
    // NULL src -> empty string
    buf[0] = 'Q';
    sanitize_copy(buf, sizeof(buf), NULL);
    assert(buf[0] == '\0');
    // cap 0 / NULL dst -> no-op, no crash
    sanitize_copy(buf, 0, "zzz");
    sanitize_copy(NULL, 16, "zzz");
}

static void test_csv_quote_field(void)
{
    char buf[32];
    // a comma is preserved INSIDE the quotes (that's the whole point — it can't split the row)
    csv_quote_field(buf, sizeof(buf), "a,b");
    assert(strcmp(buf, "\"a,b\"") == 0);
    // an embedded quote is doubled (RFC4180)
    csv_quote_field(buf, sizeof(buf), "a\"b");
    assert(strcmp(buf, "\"a\"\"b\"") == 0);
    // control bytes (newline here) -> '.', so a crafted SSID can't inject a new CSV line
    csv_quote_field(buf, sizeof(buf), "x\ny");
    assert(strcmp(buf, "\"x.y\"") == 0);
    // empty field is a bare pair of quotes
    csv_quote_field(buf, sizeof(buf), "");
    assert(strcmp(buf, "\"\"") == 0);
    // truncation: content is cut but the field stays valid — closing quote + NUL always present
    char small[6];
    small[5] = 'Z';
    csv_quote_field(small, sizeof(small), "abcdef");
    assert(small[0] == '"' && small[strlen(small) - 1] == '"' && small[5] == '\0');
    assert(strlen(small) <= 5);
    // degenerate caps -> empty string, no overflow
    char tiny[2];
    tiny[1] = 'Z';
    csv_quote_field(tiny, sizeof(tiny), "x");
    assert(tiny[0] == '\0');
    csv_quote_field(buf, 0, "x");  // no-op, no crash
    csv_quote_field(NULL, 8, "x");
}

int main(void)
{
    test_parse_mac();
    test_parse_hex_octets();
    test_parse_int_arg();
    test_sanitize_copy();
    test_csv_quote_field();
    printf("test_cliutil: all assertions passed\n");
    return 0;
}
