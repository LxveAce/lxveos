// Host-side unit test for lxveos_macro (the `;`-separated macro parser + passive-command allow-list). Pure
// libc, no ESP-IDF. Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
// This is a safety boundary: the deny cases below MUST all reject.
#include "lxveos_macro.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_accept(void)
{
    lxveos_macro_op_t ops[LXVEOS_MACRO_MAX_OPS];
    lxveos_macro_err_t err = LXVEOS_MACRO_ERR_DENIED;

    // a chain of passive verbs parses to the right command list, in order
    size_t n = lxveos_macro_parse("scan; sniff; eviltwin", ops, LXVEOS_MACRO_MAX_OPS, &err);
    assert(n == 3 && err == LXVEOS_MACRO_OK);
    assert(strcmp(ops[0].cmd, "scan") == 0);
    assert(strcmp(ops[1].cmd, "sniff") == 0);
    assert(strcmp(ops[2].cmd, "eviltwin") == 0);

    // args are preserved; surrounding whitespace and a trailing ';' are tolerated
    n = lxveos_macro_parse("  sniff 60 6 ; blescan 10 ; ", ops, LXVEOS_MACRO_MAX_OPS, &err);
    assert(n == 2 && err == LXVEOS_MACRO_OK);
    assert(strcmp(ops[0].cmd, "sniff 60 6") == 0);
    assert(strcmp(ops[1].cmd, "blescan 10") == 0);
}

static void test_deny_offensive_and_unknown(void)
{
    lxveos_macro_op_t ops[LXVEOS_MACRO_MAX_OPS];
    lxveos_macro_err_t err;
    // each of these must reject the WHOLE macro (nothing runs): offensive/arm-gated verbs, the mixed verbs
    // whose subcommands can transmit, state-changing verbs, and unknown verbs.
    static const char *const bad[] = {
        "scan; badble", "evilportal", "subghz replay", "nfc clone", "nrf24 mousejack hi",
        "arm", "disarm", "reboot", "nvs set k v", "agree", "bridge on", "ir send 4",
        "scan; frobnicate",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = LXVEOS_MACRO_OK;
        assert(lxveos_macro_parse(bad[i], ops, LXVEOS_MACRO_MAX_OPS, &err) == 0);
        assert(err == LXVEOS_MACRO_ERR_DENIED);
    }
}

static void test_edges(void)
{
    lxveos_macro_op_t ops[LXVEOS_MACRO_MAX_OPS];
    lxveos_macro_err_t err;

    // empty / whitespace-only / NULL macro -> EMPTY, zero ops
    err = LXVEOS_MACRO_OK;
    assert(lxveos_macro_parse("", ops, LXVEOS_MACRO_MAX_OPS, &err) == 0 && err == LXVEOS_MACRO_ERR_EMPTY);
    err = LXVEOS_MACRO_OK;
    assert(lxveos_macro_parse("  ;  ", ops, LXVEOS_MACRO_MAX_OPS, &err) == 0 && err == LXVEOS_MACRO_ERR_EMPTY);
    err = LXVEOS_MACRO_OK;
    assert(lxveos_macro_parse(NULL, ops, LXVEOS_MACRO_MAX_OPS, &err) == 0 && err == LXVEOS_MACRO_ERR_EMPTY);

    // more commands than fit the caller's buffer -> TOO_MANY
    err = LXVEOS_MACRO_OK;
    assert(lxveos_macro_parse("scan; sniff; stations", ops, 2, &err) == 0
           && err == LXVEOS_MACRO_ERR_TOO_MANY);

    // a single command token too long to store (allowed verb, oversize args) -> TOO_LONG
    char longtok[64];
    memcpy(longtok, "status ", 7);
    memset(longtok + 7, 'x', 50);
    longtok[57] = '\0';
    err = LXVEOS_MACRO_OK;
    assert(lxveos_macro_parse(longtok, ops, LXVEOS_MACRO_MAX_OPS, &err) == 0
           && err == LXVEOS_MACRO_ERR_TOO_LONG);
}

int main(void)
{
    test_accept();
    test_deny_offensive_and_unknown();
    test_edges();
    printf("test_macro: all tests passed\n");
    return 0;
}
