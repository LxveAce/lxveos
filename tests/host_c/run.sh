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

# --- serial-bridge event-line builder: dependency-free, no stubs needed ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_evt/include" \
    "$here/test_evt.c" "$root/components/lxveos_evt/src/lxveos_evt.c" -o "$out"
"$out"

# --- CLI helpers (validated int-arg parse + console-escape sanitize): dependency-free, no stubs needed ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_cliutil/include" \
    "$here/test_cliutil.c" "$root/components/lxveos_cliutil/src/lxveos_cliutil.c" -o "$out"
"$out"

# --- HID keystroke map (US-layout ascii->usb-hid + named keys): dependency-free, no stubs needed ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_hidmap/include" \
    "$here/test_hidmap.c" "$root/components/lxveos_hidmap/src/lxveos_hidmap.c" -o "$out"
"$out"

# --- external-radio math (CC1101 freq-word/RSSI, PN532 frame/BCC, nRF24 Unifying checksum): libc-only ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_radiomath/include" \
    "$here/test_radiomath.c" "$root/components/lxveos_radiomath/src/lxveos_radiomath.c" -o "$out"
"$out"

# --- Wi-Fi authmode label + security-grade helpers: needs the wifi_auth_mode_t enum stub ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_wifi/include" -I"$here/stubs" \
    "$here/test_wifi_labels.c" "$root/components/lxveos_wifi/src/lxveos_wifi_labels.c" -o "$out"
"$out"

# --- BLE value->label helpers (company/service/tracker/appearance): esp_err stub for lxveos_ble.h, no NimBLE ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_ble/include" -I"$root/components/lxveos_ble/src" -I"$here/stubs" \
    "$here/test_ble_labels.c" "$root/components/lxveos_ble/src/lxveos_ble_labels.c" -o "$out"
"$out"

# --- IR NEC/Sony protocol decode: pure durations->proto/addr/cmd, esp_err stub for lxveos_ir.h, no RMT ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_ir/include" -I"$here/stubs" \
    "$here/test_ir_decode.c" "$root/components/lxveos_ir/src/lxveos_ir_decode.c" -o "$out"
"$out"

# --- board panel identity: pure RDID4 0xD3 -> driver-name decision, no ESP-IDF/esp_lcd ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_board/include" \
    "$here/test_board_panel.c" "$root/components/lxveos_board/src/lxveos_board_panel.c" -o "$out"
"$out"

# --- shared-SPI3 bus refcount policy (acquire/release state machine): esp_err stub, no spi driver ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_spibus/include" -I"$here/stubs" \
    "$here/test_spibus.c" "$root/components/lxveos_spibus/src/lxveos_spibus_policy.c" -o "$out"
"$out"

# --- on-device GUI text composition (arm banner + capability menu): real op catalog + caps registry, driven
#     off a fixture sdkconfig.h (Wi-Fi board + sub-GHz add-on) so every op-status glyph is exercised ---
"$CC" -std=gnu11 -O1 -Wall \
    -I"$root/components/lxveos_gui/src" -I"$root/components/lxveos_caps/include" \
    -I"$root/components/lxveos_arm/include" -I"$here/stubs" \
    "$here/test_gui_menu.c" "$root/components/lxveos_gui/src/lxveos_gui_menu.c" \
    "$root/components/lxveos_caps/src/lxveos_ops.c" "$root/components/lxveos_caps/src/lxveos_caps.c" -o "$out"
"$out"

echo "host-c: all tests passed"
