# lxveos_caps — the capability registry: compile-time CONFIG_LXVEOS_HAS_* -> runtime-active capability
# mask. Depends only on Kconfig (global via sdkconfig.h) and esp_log (a common requirement), so no
# explicit REQUIRES. Feature components gate on lxveos_cap_active() from here.
idf_component_register(
    SRCS "src/lxveos_caps.c"
    INCLUDE_DIRS "include"
)
