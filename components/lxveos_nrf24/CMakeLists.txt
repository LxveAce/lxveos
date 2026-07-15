# lxveos_nrf24 — nRF24L01+ 2.4 GHz radio (the nrf24_scan recon op; increment 1: bring-up + RPD channel
# scan). REQUIRES esp_driver_spi (SPI master) + esp_driver_gpio (the CE line is a plain GPIO). Add-on module
# on operator-supplied pins (SPI3/VSPI to stay off the display's SPI2). Receive only in this increment.
idf_component_register(
    SRCS "src/lxveos_nrf24.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio lxveos_arm lxveos_radiomath
)
