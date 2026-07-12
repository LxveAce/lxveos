# lxveos_config — owns all LXVEOS_HAS_* Kconfig symbols and exposes the generated per-board
# board_info.h (boards/<LXVEOS_BOARD>/) on the include path project-wide.
#
# LXVEOS_BOARD comes from the ENVIRONMENT, not a -D cache variable. ESP-IDF resolves component
# requirements by re-running every component's CMakeLists in a separate `cmake -P` script process
# (component_get_requirements), and that process has no CMake cache — so a -D var set on the main
# configure is simply absent there and this check would fire. The process environment IS inherited
# by that sub-process, so an env var is the one value visible in both passes. Build with:
#   LXVEOS_BOARD=<board> idf.py -B build/<board> -D SDKCONFIG_DEFAULTS="..." build
if(NOT DEFINED ENV{LXVEOS_BOARD})
    message(FATAL_ERROR "LXVEOS_BOARD env var is not set — build with `LXVEOS_BOARD=<board> idf.py ...` (see README).")
endif()
idf_component_register(
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/boards/$ENV{LXVEOS_BOARD}"
)
