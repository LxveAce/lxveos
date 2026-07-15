// Host-side unit test for lxveos_hidmap (US-layout ascii_to_hid + named_key). Pure libc, no ESP-IDF.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion. A wrong
// keycode here would type the wrong key over BLE HID, so the whole map is checked byte-exact.
#include "lxveos_hidmap.h"

#include <assert.h>
#include <stdio.h>

#define MOD_LSHIFT 0x02

// assert ascii_to_hid(c) == (true, expected mod, expected key)
static void chk(char c, uint8_t emod, uint8_t ekey)
{
    uint8_t mod = 0xEE, key = 0xEE;
    assert(ascii_to_hid(c, &mod, &key));
    assert(mod == emod && key == ekey);
}

// assert ascii_to_hid(c) == false, and both outputs cleared to 0
static void chk_none(char c)
{
    uint8_t mod = 0xEE, key = 0xEE;
    assert(!ascii_to_hid(c, &mod, &key));
    assert(mod == 0 && key == 0);
}

static void test_letters_digits(void)
{
    chk('a', 0, 0x04);
    chk('m', 0, 0x10);           // 0x04 + 12
    chk('z', 0, 0x1D);           // 0x04 + 25
    chk('A', MOD_LSHIFT, 0x04);  // same keycode as 'a', only shifted
    chk('Z', MOD_LSHIFT, 0x1D);
    chk('1', 0, 0x1E);
    chk('5', 0, 0x22);           // 0x1E + 4
    chk('9', 0, 0x26);           // 0x1E + 8
    chk('0', 0, 0x27);           // '0' is special-cased, not contiguous with 1-9
}

static void test_whitespace_and_unshifted_punct(void)
{
    chk(' ',  0, 0x2C);
    chk('\n', 0, 0x28);  // Enter
    chk('\t', 0, 0x2B);  // Tab
    chk('-',  0, 0x2D);
    chk('=',  0, 0x2E);
    chk('[',  0, 0x2F);
    chk(']',  0, 0x30);
    chk('\\', 0, 0x31);
    chk(';',  0, 0x33);
    chk('\'', 0, 0x34);
    chk('`',  0, 0x35);
    chk(',',  0, 0x36);
    chk('.',  0, 0x37);
    chk('/',  0, 0x38);
}

static void test_shifted_punct(void)
{
    // symbol -> (shift, same keycode as the unshifted key on that physical button)
    chk('_', MOD_LSHIFT, 0x2D);  // shift of '-'
    chk('+', MOD_LSHIFT, 0x2E);  // shift of '='
    chk('{', MOD_LSHIFT, 0x2F);  // shift of '['
    chk('}', MOD_LSHIFT, 0x30);  // shift of ']'
    chk('|', MOD_LSHIFT, 0x31);  // shift of '\'
    chk(':', MOD_LSHIFT, 0x33);  // shift of ';'
    chk('"', MOD_LSHIFT, 0x34);  // shift of '\''
    chk('~', MOD_LSHIFT, 0x35);  // shift of '`'
    chk('<', MOD_LSHIFT, 0x36);  // shift of ','
    chk('>', MOD_LSHIFT, 0x37);  // shift of '.'
    chk('?', MOD_LSHIFT, 0x38);  // shift of '/'
    // shifted number row
    chk('!', MOD_LSHIFT, 0x1E);  // shift of '1'
    chk('@', MOD_LSHIFT, 0x1F);
    chk('#', MOD_LSHIFT, 0x20);
    chk('$', MOD_LSHIFT, 0x21);
    chk('%', MOD_LSHIFT, 0x22);
    chk('^', MOD_LSHIFT, 0x23);
    chk('&', MOD_LSHIFT, 0x24);
    chk('*', MOD_LSHIFT, 0x25);
    chk('(', MOD_LSHIFT, 0x26);
    chk(')', MOD_LSHIFT, 0x27);  // shift of '0'
}

static void test_shift_pairs_share_keycode(void)
{
    // A swapped keycode in the table would break this relationship: a symbol and the base key on the
    // same physical button must carry the SAME usage id, differing only by the shift modifier. The two
    // arrays are position-aligned (base[i] and its shift[i] are the same physical key).
    const char punct[]  = "-=[]\\;'`,./";
    const char pshift[] = "_+{}|:\"~<>?";
    for (int i = 0; punct[i]; i++) {
        uint8_t bm, bk, sm, sk;
        assert(ascii_to_hid(punct[i], &bm, &bk) && bm == 0);
        assert(ascii_to_hid(pshift[i], &sm, &sk) && sm == MOD_LSHIFT);
        assert(bk == sk);
    }
    const char nums[]   = "1234567890";
    const char nshift[] = "!@#$%^&*()";
    for (int i = 0; nums[i]; i++) {
        uint8_t bm, bk, sm, sk;
        assert(ascii_to_hid(nums[i], &bm, &bk) && bm == 0);
        assert(ascii_to_hid(nshift[i], &sm, &sk) && sm == MOD_LSHIFT);
        assert(bk == sk);
    }
}

static void test_unmapped_returns_false_and_clears(void)
{
    chk_none('\x01');  // a control byte
    chk_none('\x1b');  // ESC (the char; ESC is only reachable via named_key)
    chk_none('\x7f');  // DEL
    chk_none((char)0x80);  // high byte / non-ASCII
}

static void test_named_key(void)
{
    assert(named_key("ENTER") == 0x28);
    assert(named_key("RETURN") == 0x28);
    assert(named_key("enter") == 0x28);   // case-insensitive
    assert(named_key("Tab") == 0x2B);
    assert(named_key("ESC") == 0x29 && named_key("ESCAPE") == 0x29);
    assert(named_key("SPACE") == 0x2C);
    assert(named_key("BACKSPACE") == 0x2A && named_key("BKSP") == 0x2A);
    assert(named_key("DELETE") == 0x4C && named_key("DEL") == 0x4C);
    assert(named_key("UP") == 0x52 && named_key("DOWN") == 0x51);
    assert(named_key("LEFT") == 0x50 && named_key("RIGHT") == 0x4F);
    assert(named_key("HOME") == 0x4A && named_key("END") == 0x4D);
    // a single letter maps like its lowercase HID usage (GUI r, ALT F4-style scripts)
    assert(named_key("r") == 0x15);   // 0x04 + ('r' - 'a')
    assert(named_key("R") == 0x15);   // upper-case single letter, same usage
    assert(named_key("a") == 0x04 && named_key("z") == 0x1D);
    // unknown / malformed -> 0 (and no out-of-bounds read on the empty string)
    assert(named_key("FOO") == 0);
    assert(named_key("") == 0);
    assert(named_key("rr") == 0);     // multi-char non-named token
    assert(named_key(NULL) == 0);     // defensive NULL guard
}

int main(void)
{
    test_letters_digits();
    test_whitespace_and_unshifted_punct();
    test_shifted_punct();
    test_shift_pairs_share_keycode();
    test_unmapped_returns_false_and_clears();
    test_named_key();
    printf("test_hidmap: all assertions passed\n");
    return 0;
}
