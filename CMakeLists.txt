# LxveOS — root project (ESP-IDF v6.0.x, MIT). See command-center/projects/lxveos/build-architecture.md.
# The board is selected via CMakePresets.json (one preset per board), which chains the correct
# sdkconfig.defaults files (common -> chip-family -> boards/<board>/). Generate board inputs first:
#   python scripts/gen_board_configs.py     (cyd_boards.json -> boards/*, CMakePresets, board_info.h)
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lxveos)
