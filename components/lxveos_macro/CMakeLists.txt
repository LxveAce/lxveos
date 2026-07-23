# lxveos_macro — dependency-free macro parser + passive-command allow-list (see the header). Deliberately NO
# ESP-IDF REQUIRES: pure libc, so the safety-critical allow-list unit-tests on the host
# (tests/host_c/test_macro.c) and a future `macro` command can depend on it cheaply.
idf_component_register(
    SRCS "src/lxveos_macro.c"
    INCLUDE_DIRS "include"
)
