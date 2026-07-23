# lxveos_nmea — dependency-free NMEA-0183 GPS sentence parser (see the header). Deliberately NO ESP-IDF
# REQUIRES: pure libc, so it unit-tests on the host (tests/host_c/test_nmea.c) and the wardrive-log op can
# depend on it cheaply once a GPS module is wired.
idf_component_register(
    SRCS "src/lxveos_nmea.c"
    INCLUDE_DIRS "include"
)
