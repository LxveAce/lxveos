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

// Iterate the retained captured credentials (up to the last 16, oldest first), invoking cb for each pair.
// Both strings are NUL-terminated + control-sanitized.
typedef void (*lxveos_evilportal_cred_cb)(const char *user, const char *pass);
void lxveos_evilportal_creds_each(lxveos_evilportal_cred_cb cb);

// Select the captive-portal template by id (returns false if unknown; takes effect on the next request).
bool lxveos_evilportal_template_set(const char *id);
// Iterate the available templates (id + human name + whether it is the selected one), for listing.
typedef void (*lxveos_evilportal_tmpl_cb)(const char *id, const char *name, bool selected);
void lxveos_evilportal_templates_each(lxveos_evilportal_tmpl_cb cb);

#ifdef __cplusplus
}
#endif
