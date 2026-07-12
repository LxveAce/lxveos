idf_component_register(
    SRCS "src/lxveos_board.c"
    INCLUDE_DIRS "include"
    REQUIRES lxveos_config
    PRIV_REQUIRES nvs_flash
)
# M0: display/input bring-up is stubbed. When implemented, add PRIV_REQUIRES esp_lcd esp_lcd_touch
# esp_lvgl_port lvgl (pulled from the component registry into managed_components/).
