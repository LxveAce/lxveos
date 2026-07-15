#!/bin/sh
# Host-side unit tests for pure LxveOS logic that needs no ESP-IDF toolchain: the arm-gate safety state
# machine (components/lxveos_arm, against tiny esp_* stubs + a fake clock) and the untrusted-input form
# parsers (components/lxveos_formenc, dependency-free plain libc). Both compile and run under a host compiler.
# Runs in CI's `validate` job and locally: `sh tests/host_c/run.sh` (override the compiler with CC=clang).
set -eu
CC="${CC:-cc}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out="${TMPDIR:-/tmp}/lxveos_host_test"

# --- arm-gate state machine: needs the esp_* stubs and a fake, settable clock ---
build_arm() {
    "$CC" -std=gnu11 -O1 -Wall "$@" \
        -I"$root/components/lxveos_arm/include" -I"$here/stubs" \
        "$here/test_arm.c" "$root/components/lxveos_arm/src/lxveos_arm.c" -o "$out"
    "$out"
}
build_arm                      # default build: offensive TX compiled in
build_arm -DLXVEOS_TX_DISABLE  # conservative build: the emitter is stripped

# --- form/text parsers: dependency-free, no stubs needed ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_formenc/include" \
    "$here/test_formenc.c" "$root/components/lxveos_formenc/src/lxveos_formenc.c" -o "$out"
"$out"

echo "host-c: all tests passed"
