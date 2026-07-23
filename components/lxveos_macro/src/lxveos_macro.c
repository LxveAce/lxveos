// lxveos_macro — see lxveos_macro.h. Dependency-free macro parser + passive-command allow-list, host-tested
// off-target (tests/host_c/test_macro.c). libc-only, no allocation.
#include "lxveos_macro.h"

#include <string.h>  // strlen / memcmp / memcpy

// The passive command verbs a macro may chain: recon (scan/sniff/capture/wardrive/...), defense (the
// detectors), and read-only info. Everything else is rejected by omission — the offensive/arm-gated verbs,
// the mixed verbs whose subcommands can transmit (ir/subghz/nrf24/nfc), and the state-changing ones
// (arm/disarm/agree/bridge/loglevel/reboot/nvs) are all absent on purpose. Grounded in register_commands.
static const char *const MACRO_ALLOW[] = {
    "scan", "sniff", "stations", "probes", "capture", "blescan", "btracker", "airspace",
    "wardrive", "blewardrive", "defend", "pwnwatch", "eviltwin", "apaudit", "watch", "bleflood",
    "flipper", "meta", "skimmer", "flock", "surveil", "blehid", "status", "features", "caps",
    "sysinfo", "info",
};

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static int verb_allowed(const char *v, size_t vlen)
{
    if (vlen == 0) {
        return 0;
    }
    for (size_t k = 0; k < sizeof(MACRO_ALLOW) / sizeof(MACRO_ALLOW[0]); k++) {
        if (strlen(MACRO_ALLOW[k]) == vlen && memcmp(MACRO_ALLOW[k], v, vlen) == 0) {
            return 1;
        }
    }
    return 0;
}

size_t lxveos_macro_parse(const char *macro, lxveos_macro_op_t *out, size_t max, lxveos_macro_err_t *err)
{
    lxveos_macro_err_t e = LXVEOS_MACRO_OK;
    size_t nops = 0;
    if (out != NULL && macro != NULL) {
        const char *p = macro;
        for (;;) {
            const char *tok = p;
            while (*p != '\0' && *p != ';') {
                p++;
            }
            const char *tok_end = p;
            int more = (*p == ';');
            if (more) {
                p++;
            }
            // trim surrounding whitespace
            while (tok < tok_end && is_space(*tok)) {
                tok++;
            }
            while (tok_end > tok && is_space(tok_end[-1])) {
                tok_end--;
            }
            size_t tlen = (size_t)(tok_end - tok);
            if (tlen > 0) {
                if (nops >= max || nops >= LXVEOS_MACRO_MAX_OPS) {
                    e = LXVEOS_MACRO_ERR_TOO_MANY;
                    break;
                }
                if (tlen >= LXVEOS_MACRO_CMD_MAX) {
                    e = LXVEOS_MACRO_ERR_TOO_LONG;
                    break;
                }
                // the verb is the token's first word
                const char *ve = tok;
                while (ve < tok_end && !is_space(*ve)) {
                    ve++;
                }
                if (!verb_allowed(tok, (size_t)(ve - tok))) {
                    e = LXVEOS_MACRO_ERR_DENIED;
                    break;
                }
                memcpy(out[nops].cmd, tok, tlen);
                out[nops].cmd[tlen] = '\0';
                nops++;
            }
            if (!more) {
                break;
            }
        }
    } else {
        e = LXVEOS_MACRO_ERR_EMPTY;
    }

    if (e != LXVEOS_MACRO_OK) {
        if (err != NULL) {
            *err = e;
        }
        return 0;  // reject the whole macro: nothing runs
    }
    if (nops == 0) {
        e = LXVEOS_MACRO_ERR_EMPTY;
    }
    if (err != NULL) {
        *err = e;
    }
    return nops;
}
