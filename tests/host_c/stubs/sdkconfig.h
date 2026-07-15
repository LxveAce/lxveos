#pragma once
// Host-test stub for the ESP-IDF-generated sdkconfig.h. The real one is produced per board by the build from
// Kconfig + boards/<id>/sdkconfig.defaults; here it is a fixed FIXTURE so lxveos_caps.c / lxveos_ops.c compile
// and run under a host compiler. It models a "Wi-Fi board with a sub-GHz add-on": Wi-Fi built in, CC1101
// attachable — enough for test_gui_menu to see every op-status glyph ([+]/[.] Wi-Fi, [~] sub-GHz, [x] the
// rest). NOT used by the firmware build. Keep the exercised caps here in step with lxveos_config/Kconfig.
#define CONFIG_LXVEOS_HAS_WIFI 1
#define CONFIG_LXVEOS_ADDON_SUBGHZ 1
