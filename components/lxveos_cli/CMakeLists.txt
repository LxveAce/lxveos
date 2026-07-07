idf_component_register(
    SRCS "src/lxveos_cli.c"
    INCLUDE_DIRS "include"
    REQUIRES lxveos_config lxveos_board
)
# M1: add PRIV_REQUIRES console (esp_console REPL) + the Cyber Controller bridge protocol.
