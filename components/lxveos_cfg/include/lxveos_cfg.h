#pragma once
// lxveos_cfg — dependency-free (libc-only) config serializer: pack a set of {key, value} rows into one
// escaped text blob and parse it back, for the `nvs export` / `nvs import` operator-config backup. Kept a
// standalone component so the codec host-tests off-target (tests/host_c/test_cfg.c). The NVS glue that gathers
// the rows (the user keys + the watchlist) and re-applies them lives in the CLI and is compile-checked by the
// board matrix; only the escaped-text codec is here.
//
// Wire format: one row per line, "key<TAB>value<LF>". A backslash escapes the three bytes that would
// otherwise be structural, so an embedded tab / newline / backslash survives the round-trip: "\\" -> '\',
// "\t" -> TAB, "\n" -> LF. Any other backslash escape, a raw control byte, a missing key/value separator, or
// an over-long field makes the whole blob malformed (parse returns 0) rather than silently loading garbage.
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXVEOS_CFG_KEY_MAX   16   // key buffer incl. NUL (NVS keys are capped at 15 chars)
#define LXVEOS_CFG_VAL_MAX   64   // value buffer incl. NUL
#define LXVEOS_CFG_ROWS_MAX  24   // rows per blob

// A buffer of this size always holds the serialization of up to LXVEOS_CFG_ROWS_MAX rows: every key/value
// byte can escape to two, plus the two separators per row and the final NUL.
#define LXVEOS_CFG_BLOB_MAX \
    (LXVEOS_CFG_ROWS_MAX * (2 * LXVEOS_CFG_KEY_MAX + 2 * LXVEOS_CFG_VAL_MAX + 2) + 1)

typedef struct {
    char key[LXVEOS_CFG_KEY_MAX];    // NUL-terminated
    char value[LXVEOS_CFG_VAL_MAX];  // NUL-terminated
} lxveos_cfg_row_t;

// Serialize the first `n` rows into out[cap] (NUL-terminated). Returns the byte count written (excluding the
// NUL), or 0 if rows/out is NULL, cap is 0, `n` exceeds LXVEOS_CFG_ROWS_MAX, or the escaped text does not fit.
size_t lxveos_cfg_serialize(const lxveos_cfg_row_t *rows, size_t n, char *out, size_t cap);

// Parse an escaped blob (length `len`) back into rows[0..max). Returns the row count recovered (<= max and
// <= LXVEOS_CFG_ROWS_MAX). Returns 0 for an empty blob OR a malformed one (a bad escape, a raw control byte,
// a row with no key/value separator, or an over-long key/value) — so a corrupt import applies nothing.
size_t lxveos_cfg_parse(const char *blob, size_t len, lxveos_cfg_row_t *rows, size_t max);

#ifdef __cplusplus
}
#endif
