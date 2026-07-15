# lxveos_nfc — NFC PN532 reader (the nfc_read recon op + the arm-gated nfc_clone offensive op).
# REQUIRES esp_driver_i2c (the ESP-IDF v6 i2c_master driver) + lxveos_arm (the offensive-TX gate that
# nfc_clone's card write is guarded by). Add-on module on operator-supplied SDA/SCL.
idf_component_register(
    SRCS "src/lxveos_nfc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c lxveos_arm lxveos_radiomath
)
