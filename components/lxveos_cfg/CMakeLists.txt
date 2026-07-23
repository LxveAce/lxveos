# lxveos_cfg — dependency-free escaped-text config codec (see the header). Deliberately NO ESP-IDF REQUIRES:
# pure libc, so it unit-tests on the host (tests/host_c/test_cfg.c) and the CLI's nvs export/import glue can
# depend on it cheaply.
idf_component_register(
    SRCS "src/lxveos_cfg.c"
    INCLUDE_DIRS "include"
)
