# lxveos_ble — passive BLE recon (M1). A NimBLE GAP observer; the host is brought up lazily on first scan.
# REQUIRES the Bluetooth component (NimBLE host + controller); BT is enabled for every board in the common
# sdkconfig.defaults, so `bt` is always present. LISTEN-ONLY — advertising roles are compiled out.
idf_component_register(
    SRCS "src/lxveos_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
)
