# lxveos_evt — builds "LXVEOS/1 <type> k=v ..." serial-bridge event lines (see the header). Deliberately NO
# ESP-IDF REQUIRES: pure libc, so any component can depend on it cheaply and it host-tests with no toolchain
# (tests/host_c/test_evt.c).
idf_component_register(
    SRCS "src/lxveos_evt.c"
    INCLUDE_DIRS "include"
)
