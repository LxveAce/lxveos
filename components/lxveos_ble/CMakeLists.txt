# lxveos_ble — BLE recon (passive NimBLE GAP observer) + the arm-gated HID keystroke-injection op
# (lxveos_ble_hid.c, "BadBLE"). One NimBLE host, owned by lxveos_ble.c and shared by both. REQUIRES the
# Bluetooth component (NimBLE host + controller; `bt` is enabled for every board in sdkconfig.defaults) and
# lxveos_arm for the transmit gate the injection op checks before advertising.
idf_component_register(
    SRCS "src/lxveos_ble.c" "src/lxveos_ble_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES bt lxveos_arm
)
