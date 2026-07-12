idf_component_register(
    SRCS "src/lxveos_cli.c"
    INCLUDE_DIRS "include"
    REQUIRES lxveos_config lxveos_board
    PRIV_REQUIRES lxveos_caps console nvs_flash
)
# M1: the Cyber Controller bridge protocol (versioned single-line UART) lands here alongside the REPL.
