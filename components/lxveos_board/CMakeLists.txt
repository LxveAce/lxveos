idf_component_register(
    SRCS "src/lxveos_board.c"
    INCLUDE_DIRS "include"
    REQUIRES lxveos_config
    PRIV_REQUIRES nvs_flash esp_lcd esp_driver_spi esp_driver_gpio
)
# esp_lcd + esp_driver_spi (both in-tree) back the SPI panel-IO bring-up. Still TODO as bring-up grows:
# the ILI9341 panel driver is a managed component (espressif/esp_lcd_ili9341 -> main/idf_component.yml +
# PRIV_REQUIRES esp_lcd_ili9341); ST7789 is built into esp_lcd. Touch/LVGL add esp_lcd_touch esp_lvgl_port lvgl.
