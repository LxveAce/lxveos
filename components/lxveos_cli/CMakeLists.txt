idf_component_register(
    SRCS "src/lxveos_cli.c"
    INCLUDE_DIRS "include"
    REQUIRES lxveos_config lxveos_board
    PRIV_REQUIRES lxveos_caps lxveos_wifi lxveos_ble lxveos_arm lxveos_evilportal lxveos_ir console nvs_flash esp_timer
)
# M1: the Cyber Controller bridge protocol (versioned single-line UART) lands here alongside the REPL.
