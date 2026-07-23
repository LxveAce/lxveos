#pragma once
// lxveos_macro — dependency-free (libc-only) parser + allow-list validator for a `;`-separated command macro.
// It is a SAFETY boundary: a macro may chain ONLY passive recon/defense/logging commands. It is deny-by-
// default — any verb not on the compile-time allow-list is rejected, which covers the offensive/arm-gated
// verbs (evilportal, badble, arm), the mixed verbs that have an offensive subcommand (ir/subghz/nrf24/nfc,
// e.g. `subghz replay` or `nfc clone`), the state-changing ones (reboot, nvs, agree, bridge, disarm,
// loglevel), and anything unknown. Pure logic, so it host-tests off-target (tests/host_c/test_macro.c). The
// runtime that dispatches the validated command list to the existing passive command funcs is a follow-up.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXVEOS_MACRO_MAX_OPS  16   // most commands one macro may chain
#define LXVEOS_MACRO_CMD_MAX  48   // per-command buffer incl. NUL (verb + its args)

typedef struct {
    char cmd[LXVEOS_MACRO_CMD_MAX];  // the full trimmed command (verb + args), NUL-terminated
} lxveos_macro_op_t;

typedef enum {
    LXVEOS_MACRO_OK = 0,        // parsed fine; out holds the ops and the returned count is > 0
    LXVEOS_MACRO_ERR_EMPTY,     // no commands (a NULL / empty / whitespace-only macro)
    LXVEOS_MACRO_ERR_DENIED,    // a command verb is not on the passive allow-list (offensive/unknown)
    LXVEOS_MACRO_ERR_TOO_MANY,  // more commands than fit in `max` (or LXVEOS_MACRO_MAX_OPS)
    LXVEOS_MACRO_ERR_TOO_LONG,  // a single command token is too long to store
} lxveos_macro_err_t;

// Parse + validate a `;`-separated macro. Each token is trimmed of surrounding whitespace; empty tokens (a
// trailing `;`, `;;`) are skipped. Every non-empty token's VERB (its first word) must be on the passive
// allow-list, or the WHOLE macro is rejected. On success writes the validated commands into out[0..max) and
// returns the count (>= 1). On any error returns 0 (nothing should run) and sets *err (may be NULL) to why.
size_t lxveos_macro_parse(const char *macro, lxveos_macro_op_t *out, size_t max, lxveos_macro_err_t *err);

#ifdef __cplusplus
}
#endif
