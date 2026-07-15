# lxveos_subghz — sub-GHz CC1101 radio (the subghz_scan op; increment 1: bring-up + identity + RSSI sense).
# REQUIRES esp_driver_spi (the SPI master driver). The CC1101 is an add-on module on operator-supplied SPI
# pins (SPI3/VSPI host, to stay off the display's SPI2). Receive only in this increment.
idf_component_register(
    SRCS "src/lxveos_subghz.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_rmt lxveos_arm lxveos_radiomath
)
