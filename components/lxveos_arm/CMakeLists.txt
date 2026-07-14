# lxveos_arm — the arm/safety framework: two-factor arm + inactivity timeout gate for offensive-TX ops,
# on top of the compile-time LXVEOS_TX_ENABLE switch (unset in every committed board config). esp_timer
# for the monotonic clock; esp_log is a common requirement (available without an explicit REQUIRES).
idf_component_register(
    SRCS "src/lxveos_arm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
