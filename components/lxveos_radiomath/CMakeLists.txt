# lxveos_radiomath — pure, dependency-free math for the external-radio drivers (CC1101 sub-GHz freq-word +
# RSSI, PN532 NFC frame/BCC, nRF24 Unifying checksum). No SPI/I2C/RF; host-testable off-target
# (tests/host_c/test_radiomath.c). The subghz / nfc / nrf24 drivers REQUIRE this and call into it so the
# firmware and the host tests share one source of truth for the register/frame arithmetic.
idf_component_register(
    SRCS "src/lxveos_radiomath.c"
    INCLUDE_DIRS "include"
)
