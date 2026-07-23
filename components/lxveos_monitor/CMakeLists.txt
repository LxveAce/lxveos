# lxveos_monitor — dependency-free passive-capture rotation planner (see the header). Deliberately NO ESP-IDF
# REQUIRES: pure libc, so it unit-tests on the host (tests/host_c/test_monitor.c) and a future `monitor`
# command can depend on it cheaply.
idf_component_register(
    SRCS "src/lxveos_monitor.c"
    INCLUDE_DIRS "include"
)
