# lxveos_baseline — dependency-free environment-fingerprint baseline set/diff/codec (see the header).
# Deliberately NO ESP-IDF REQUIRES: pure libc, so it unit-tests on the host (tests/host_c/test_baseline.c) and
# a future `baseline` op can depend on it cheaply once the scan + NVS-persistence path is wired (the lxveos_pcap
# pattern: pure core now, op wired later).
idf_component_register(
    SRCS "src/lxveos_baseline.c"
    INCLUDE_DIRS "include"
)
