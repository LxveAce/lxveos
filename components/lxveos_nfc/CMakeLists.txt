# lxveos_nfc — NFC PN532 reader (the nfc_read recon op; increment 1: identify + read a card UID over I2C).
# REQUIRES esp_driver_i2c (the ESP-IDF v6 i2c_master driver). Add-on module on operator-supplied SDA/SCL.
# Read only in this increment (no write/emulate).
idf_component_register(
    SRCS "src/lxveos_nfc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
