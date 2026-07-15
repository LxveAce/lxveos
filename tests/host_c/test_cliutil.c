// Host-side unit test for lxveos_cliutil (parse_int_arg + sanitize_copy). Pure libc, no ESP-IDF toolchain.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_cliutil.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    test_parse_int_arg();
    test_sanitize_copy();
    test_csv_quote_field();
    printf("test_cliutil: all assertions passed\n");
    return 0;
}
