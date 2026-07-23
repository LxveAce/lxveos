# lxveos_pcap — dependency-free PCAP header encoder (see the header). Deliberately NO ESP-IDF REQUIRES: pure
# libc, so it unit-tests on the host (tests/host_c/test_pcap.c) and the pcap_log op can depend on it cheaply
# once the SD storage path is wired.
idf_component_register(
    SRCS "src/lxveos_pcap.c"
    INCLUDE_DIRS "include"
)
