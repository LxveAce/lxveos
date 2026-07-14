idf_component_register(
    SRCS "lxveos_main.c"
    INCLUDE_DIRS "."
    REQUIRES lxveos_config lxveos_board lxveos_caps lxveos_cli lxveos_gui
    PRIV_REQUIRES nvs_flash
)
