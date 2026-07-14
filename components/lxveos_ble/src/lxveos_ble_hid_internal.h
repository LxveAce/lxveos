#pragma once
// Internal seam between the BLE component's NimBLE host owner (lxveos_ble.c) and the arm-gated HID
// keyboard-injection peripheral (lxveos_ble_hid.c). NimBLE is a process-wide singleton and lxveos_ble.c
// owns its lifecycle, so the HID GATT services must be registered from inside that one bring-up, before
// the host task starts. This header lets lxveos_ble.c call into the HID module for exactly that, without
// exposing HID internals in the public header.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the HID-over-GATT services (GAP + GATT base services, the HID service, and Device Information).
// MUST be called from ensure_host_up() AFTER ble_store_config_init() and BEFORE nimble_port_freertos_init()
// — GATT services have to exist before the host starts them at sync. Idempotent. Returns 0 on success or a
// NimBLE return code. Registering the services does NOT transmit anything; nothing goes on-air until an
// arm-gated lxveos_ble_hid_inject() begins advertising.
int lxveos_ble_hid_services_register(void);

// Implemented in lxveos_ble.c (the NimBLE host owner): bring the host up if needed and resolve our own
// address type into *own_addr_type. Lets the HID module start advertising without duplicating the host
// bring-up. Returns ESP_OK or an esp_err_t on init/identity failure.
esp_err_t lxveos_ble_hid_host_ready(uint8_t *own_addr_type);

#ifdef __cplusplus
}
#endif
