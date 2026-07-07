# lxveos_config — owns all LXVEOS_HAS_* Kconfig symbols and exposes the generated per-board
# board_info.h (boards/<LXVEOS_BOARD>/) on the include path project-wide. LXVEOS_BOARD is set by the
# CMakePresets entry for the selected board.
if(NOT DEFINED LXVEOS_BOARD)
    message(FATAL_ERROR "LXVEOS_BOARD is not set — build via a CMakePresets preset (idf.py --preset <board> build).")
endif()
idf_component_register(
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/boards/${LXVEOS_BOARD}"
)
