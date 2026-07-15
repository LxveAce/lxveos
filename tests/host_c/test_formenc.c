// Host-side unit test for the form/text parsers in components/lxveos_formenc. These run on client-supplied
// captive-portal POST bodies (untrusted input), so their edge cases — a lone or malformed '%', an embedded
// decoded NUL, a whole-token key match vs a prefix, bounded truncation, and the dstsz==0 guard — are exactly
// where a silent regression would matter. The component is dependency-free (plain libc), so this compiles and
// runs under a host compiler with no ESP-IDF stubs. See tests/host_c/run.sh.
#include <stdio.h>
#include <string.h>

#include "lxveos_formenc.h"

static int g_fail = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

static void test_url_decode(void)
{
    char b[64];

    lxveos_formenc_url_decode("a+b", b, sizeof(b));
    CHECK(strcmp(b, "a b") == 0);  // '+' -> space

    lxveos_formenc_url_decode("%41%42%43", b, sizeof(b));
    CHECK(strcmp(b, "ABC") == 0);  // %XX -> byte

    lxveos_formenc_url_decode("hello%20world", b, sizeof(b));
    CHECK(strcmp(b, "hello world") == 0);

    // %2B decodes to a literal '+' byte, distinct from a bare '+' (which becomes a space).
    lxveos_formenc_url_decode("a%2Bb", b, sizeof(b));
    CHECK(strcmp(b, "a+b") == 0);

    // A '%' not followed by two hex digits is copied through literally (lone, short, and non-hex forms).
    lxveos_formenc_url_decode("abc%", b, sizeof(b));
    CHECK(strcmp(b, "abc%") == 0);
    lxveos_formenc_url_decode("x%2", b, sizeof(b));
    CHECK(strcmp(b, "x%2") == 0);
    lxveos_formenc_url_decode("%zz", b, sizeof(b));
    CHECK(strcmp(b, "%zz") == 0);
    lxveos_formenc_url_decode("%4x", b, sizeof(b));  // first nibble hex, second not
    CHECK(strcmp(b, "%4x") == 0);

    // A decoded NUL is preserved in the buffer (so dst can carry embedded NULs), then the string is
    // NUL-terminated as usual after the last written byte.
    memset(b, 'Z', sizeof(b));
    lxveos_formenc_url_decode("x%00y", b, sizeof(b));
    CHECK(b[0] == 'x' && b[1] == '\0' && b[2] == 'y' && b[3] == '\0');

    // Bounded: a 4-byte destination holds 3 chars + terminator, no overflow.
    char small[4];
    lxveos_formenc_url_decode("abcdefgh", small, sizeof(small));
    CHECK(strcmp(small, "abc") == 0);

    // dstsz == 0: writes nothing, leaves the buffer untouched.
    char canary[8];
    memset(canary, 'C', sizeof(canary));
    lxveos_formenc_url_decode("whatever", canary, 0);
    for (size_t i = 0; i < sizeof(canary); i++) {
        CHECK(canary[i] == 'C');
    }
}

static void test_form_field(void)
{
    char v[64];

    CHECK(lxveos_formenc_form_field("username=alice&password=secret", "username", v, sizeof(v)));
    CHECK(strcmp(v, "alice") == 0);
    CHECK(lxveos_formenc_form_field("username=alice&password=secret", "password", v, sizeof(v)));
    CHECK(strcmp(v, "secret") == 0);

    // Absent key -> false, and `out` is left untouched.
    strcpy(v, "SENTINEL");
    CHECK(lxveos_formenc_form_field("username=alice", "missing", v, sizeof(v)) == false);
    CHECK(strcmp(v, "SENTINEL") == 0);

    // Whole-token match only: "user" must not match the "username=" token.
    CHECK(lxveos_formenc_form_field("user=1&username=2", "user", v, sizeof(v)));
    CHECK(strcmp(v, "1") == 0);
    CHECK(lxveos_formenc_form_field("user=1&username=2", "username", v, sizeof(v)));
    CHECK(strcmp(v, "2") == 0);

    // The value is URL-decoded (both %XX and '+').
    CHECK(lxveos_formenc_form_field("u=a%20b", "u", v, sizeof(v)));
    CHECK(strcmp(v, "a b") == 0);
    CHECK(lxveos_formenc_form_field("q=a+b", "q", v, sizeof(v)));
    CHECK(strcmp(v, "a b") == 0);

    // Key found as a later token, and an empty value.
    CHECK(lxveos_formenc_form_field("x=9&user=bob", "user", v, sizeof(v)));
    CHECK(strcmp(v, "bob") == 0);
    CHECK(lxveos_formenc_form_field("u=&v=1", "u", v, sizeof(v)));
    CHECK(strcmp(v, "") == 0);
}

static void test_sanitize(void)
{
    char s1[] = "a\tb\nc";
    lxveos_formenc_sanitize(s1);
    CHECK(strcmp(s1, "a.b.c") == 0);

    char s2[] = "x\x7fy";  // DEL
    lxveos_formenc_sanitize(s2);
    CHECK(strcmp(s2, "x.y") == 0);

    char s3[] = "Hello World!";  // printable ASCII unchanged
    lxveos_formenc_sanitize(s3);
    CHECK(strcmp(s3, "Hello World!") == 0);

    // High bytes (>= 0x80) are not control bytes here and are left as-is (documents current behaviour).
    char s4[] = {'a', (char)0x80, 'b', '\0'};
    lxveos_formenc_sanitize(s4);
    CHECK(s4[0] == 'a' && (unsigned char)s4[1] == 0x80 && s4[2] == 'b');
}

static void test_store_field(void)
{
    char b[16];

    lxveos_formenc_store_field(b, sizeof(b), "hello");
    CHECK(strcmp(b, "hello") == 0);

    // Truncation: 4-byte buffer keeps 3 chars + terminator.
    char t[4];
    lxveos_formenc_store_field(t, sizeof(t), "abcdefgh");
    CHECK(strcmp(t, "abc") == 0);

    // Exact fit (len == dstsz-1) copies fully.
    char e[4];
    lxveos_formenc_store_field(e, sizeof(e), "abc");
    CHECK(strcmp(e, "abc") == 0);

    // dstsz == 0: no-op, buffer untouched.
    char canary[8];
    memset(canary, 'C', sizeof(canary));
    lxveos_formenc_store_field(canary, 0, "overflow me");
    for (size_t i = 0; i < sizeof(canary); i++) {
        CHECK(canary[i] == 'C');
    }
}

int main(void)
{
    test_url_decode();
    test_form_field();
    test_sanitize();
    test_store_field();
    printf(g_fail ? "formenc host tests: FAILED\n" : "formenc host tests: OK\n");
    return g_fail;
}
