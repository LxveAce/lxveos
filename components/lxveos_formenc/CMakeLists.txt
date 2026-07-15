# lxveos_formenc — dependency-free helpers for handling untrusted form-submission text (see the header).
# Deliberately NO ESP-IDF REQUIRES: pure libc, so it unit-tests on the host (tests/host_c/test_formenc.c)
# and any component can depend on it cheaply.
idf_component_register(
    SRCS "src/lxveos_formenc.c"
    INCLUDE_DIRS "include"
)
