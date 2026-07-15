# lxveos_cliutil — dependency-free CLI helpers (validated int-arg parse + console-escape sanitize). No
# ESP-IDF REQUIRES: pure libc, so any component can depend on it cheaply and it host-tests with no toolchain
# (tests/host_c/test_cliutil.c).
idf_component_register(
    SRCS "src/lxveos_cliutil.c"
    INCLUDE_DIRS "include"
)
