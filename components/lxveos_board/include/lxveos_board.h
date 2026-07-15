#pragma once
// LxveOS board bring-up entry. The board is selected at build time via LXVEOS_BOARD (exported env var);
// its details come from the generated boards/<board>/board_info.h. See README.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the selected board (display + input where present). Safe on headless boards.
esp_err_t lxveos_board_init(void);

const char *lxveos_board_id(void);
const char *lxveos_board_chip(void);   // esp32 | esp32s3 | ... (the CONFIG_IDF_TARGET this image was built for)
const char *lxveos_ui_profile(void);   // headless | button_gui | keypad_gui | encoder_gui | touch_gui

#ifdef __cplusplus
}
#endif
