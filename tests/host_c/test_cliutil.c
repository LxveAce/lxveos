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

int main(void)
{
    test_parse_int_arg();
    test_sanitize_copy();
    printf("test_cliutil: all assertions passed\n");
    return 0;
}
