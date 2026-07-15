# lxveos_spibus — refcounted shared SPI3 bus manager for the CC1101 (subghz) + nRF24 add-ons, so neither
# frees the bus while the other still holds it. The pure refcount policy (lxveos_spibus_policy.c) is
# host-tested off-target; lxveos_spibus.c drives the spi_bus_* side effects from it. REQUIRES esp_driver_spi.
idf_component_register(
    SRCS "src/lxveos_spibus.c" "src/lxveos_spibus_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi
)
