# lxveos_arm — the arm/safety framework: two-factor arm + inactivity timeout gate for offensive-TX ops.
# Offensive TX is compiled IN by default; a conservative/public build defines LXVEOS_TX_DISABLE to strip the
# emitters (then lxveos_arm_can_emit() is a hard false). esp_timer for the monotonic clock; esp_log is a
# common requirement (available without an explicit REQUIRES).
idf_component_register(
    SRCS "src/lxveos_arm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
