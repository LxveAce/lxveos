// Host-side unit test for lxveos_cfg (the escaped-text {key,value} config codec). Pure libc, no ESP-IDF.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion. The core check is
// a round-trip: serialize a set of rows (with the structural bytes embedded as data), parse it back, and
// require the rows to come out identical.
#include "lxveos_cfg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_roundtrip(void)
{
    lxveos_cfg_row_t in[3];
    memset(in, 0, sizeof(in));
    strcpy(in[0].key, "ssid");
    strcpy(in[0].value, "MyNet");
    strcpy(in[1].key, "note");
    strcpy(in[1].value, "tab\there and\nnewline");  // embedded field + row separators must survive
    strcpy(in[2].key, "path");
    strcpy(in[2].value, "a\\b\\c");                  // embedded backslashes

    char blob[256];
    size_t bn = lxveos_cfg_serialize(in, 3, blob, sizeof(blob));
    assert(bn > 0);

    lxveos_cfg_row_t out[3];
    memset(out, 0, sizeof(out));
    size_t rn = lxveos_cfg_parse(blob, bn, out, 3);
    assert(rn == 3);
    for (int i = 0; i < 3; i++) {
        assert(strcmp(out[i].key, in[i].key) == 0);
        assert(strcmp(out[i].value, in[i].value) == 0);
    }

    // parse honours the caller's row cap
    assert(lxveos_cfg_parse(blob, bn, out, 2) == 2);
}

static void test_empty_and_limits(void)
{
    lxveos_cfg_row_t rows[LXVEOS_CFG_ROWS_MAX];
    char blob[16];

    // zero rows -> an empty blob (0 bytes) that parses back to zero rows
    assert(lxveos_cfg_serialize(rows, 0, blob, sizeof(blob)) == 0);
    assert(lxveos_cfg_parse("", 0, rows, LXVEOS_CFG_ROWS_MAX) == 0);

    lxveos_cfg_row_t one[1];
    memset(one, 0, sizeof(one));
    strcpy(one[0].key, "k");
    strcpy(one[0].value, "v");
    // n over the row cap is rejected
    assert(lxveos_cfg_serialize(one, LXVEOS_CFG_ROWS_MAX + 1, blob, sizeof(blob)) == 0);
    // a too-small output buffer returns 0 (no partial write, no overflow)
    char tiny[4];
    assert(lxveos_cfg_serialize(one, 1, tiny, sizeof(tiny)) == 0);
}

static void test_malformed(void)
{
    lxveos_cfg_row_t out[4];
    // a good blob is the baseline
    assert(lxveos_cfg_parse("k\tv\n", 4, out, 4) == 1);
    assert(strcmp(out[0].key, "k") == 0 && strcmp(out[0].value, "v") == 0);
    // every one of these is malformed -> 0 rows (a corrupt import applies nothing)
    assert(lxveos_cfg_parse("k\tv\\x\n", 6, out, 4) == 0);   // unknown escape
    assert(lxveos_cfg_parse("keyonly\n", 8, out, 4) == 0);   // no key/value separator
    assert(lxveos_cfg_parse("k\tv\x01x\n", 7, out, 4) == 0); // raw control byte in a value
    assert(lxveos_cfg_parse("k\tv", 3, out, 4) == 0);        // unterminated row (no newline)
    assert(lxveos_cfg_parse("k\tv\\", 4, out, 4) == 0);      // trailing backslash, nothing after
}

int main(void)
{
    test_roundtrip();
    test_empty_and_limits();
    test_malformed();
    printf("test_cfg: all tests passed\n");
    return 0;
}
