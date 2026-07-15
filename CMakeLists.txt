# LxveOS — root project (ESP-IDF v6.0.x, MIT).
# The board is selected with explicit idf.py flags: export LXVEOS_BOARD=<board> and pass the chained
# SDKCONFIG_DEFAULTS (common -> chip-family -> boards/<board>/sdkconfig.defaults). Generate board inputs first:
#   python scripts/gen_board_configs.py     (cyd_boards.json -> boards/*/ sdkconfig.defaults + board_info.h)
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lxveos)
