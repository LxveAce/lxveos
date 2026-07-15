#!/bin/sh
# Host-side unit tests for the arm-gate safety state machine (components/lxveos_arm). Compiles lxveos_arm.c
# against the tiny ESP-IDF stubs in stubs/ (with a fake, settable clock) and runs it under a plain host
# compiler — no ESP-IDF toolchain needed. Runs in CI's `validate` job and locally: `sh tests/host_c/run.sh`
# (override the compiler with CC=clang, etc.).
set -eu
CC="${CC:-cc}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out="${TMPDIR:-/tmp}/lxveos_test_arm"

build_and_run() {
    "$CC" -std=gnu11 -O1 -Wall "$@" \
        -I"$root/components/lxveos_arm/include" -I"$here/stubs" \
        "$here/test_arm.c" "$root/components/lxveos_arm/src/lxveos_arm.c" -o "$out"
    "$out"
}

build_and_run                     # default build: offensive TX compiled in
build_and_run -DLXVEOS_TX_DISABLE  # conservative build: the emitter is stripped
echo "host-c: arm-gate tests passed"
