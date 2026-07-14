idf_component_register(
    SRCS "src/lxveos_gui.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lxveos_board lxveos_caps esp_lcd lvgl esp_lvgl_port
)
# lvgl + esp_lvgl_port are managed components (see idf_component.yml). The GUI drives the board's esp_lcd
# panel handle (bsp_display_panel_handle) through the LVGL port and renders the launcher from the op
# catalog (lxveos_caps). On headless boards lxveos_gui_start() no-ops.
