# lxveos_ir — IR capture + replay (the ir_recv / ir_send ops), a Flipper-class universal remote via the
# RMT peripheral. REQUIRES esp_driver_rmt (the RMT RX/TX driver in ESP-IDF v6). Needs an IR receiver on one
# GPIO and an IR LED on another (operator-supplied); no on-board radio.
idf_component_register(
    SRCS "src/lxveos_ir.c" "src/lxveos_ir_decode.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt
)
