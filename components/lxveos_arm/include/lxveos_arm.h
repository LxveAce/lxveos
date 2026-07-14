#pragma once
// LxveOS arm / safety framework — the gate for any offensive-TX operation.
//
// Two independent layers protect the transmit path:
//  1. COMPILE-TIME: the offensive-TX path is compiled out unless the image is built with LXVEOS_TX_ENABLE.
//     No committed board config sets it, so a released unit has no offensive emitter at all and
//     lxveos_arm_can_emit() is a hard false — arming is a no-op there. The owner sets the flag and drives
//     emission only on the licensed, RF-shielded bench.
//  2. RUNTIME: even in a TX-enabled build, an op may transmit only after a two-factor arm (request ->
//     confirm with a one-time token) and only until an explicit disarm or an inactivity timeout.
//
// Passive recon / defense / logging ops never touch this; only ops that put energy on-air call
// lxveos_arm_can_emit() immediately before transmitting.
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LXVEOS_ARM_SAFE = 0,   // default: no offensive TX permitted
    LXVEOS_ARM_PENDING,    // arm requested; awaiting the confirm token (two-factor)
    LXVEOS_ARM_ARMED       // armed; offensive-TX ops may run until disarm / timeout
} lxveos_arm_state_t;

// True iff this image was BUILT with offensive TX compiled in (LXVEOS_TX_ENABLE). Released images: false.
bool lxveos_arm_tx_compiled(void);

lxveos_arm_state_t lxveos_arm_state(void);
const char *lxveos_arm_state_name(lxveos_arm_state_t s);

// Begin arming. On success fills *token with a one-time confirm code the operator must echo back within the
// confirm window. ESP_ERR_NOT_SUPPORTED if TX is not compiled in. Moves state SAFE/ARMED -> PENDING.
esp_err_t lxveos_arm_request(uint32_t *token);

// Second factor: state PENDING -> ARMED only if `token` matches the request and the window has not expired.
// Any failure (wrong token, expired, wrong state) leaves/returns the state SAFE.
esp_err_t lxveos_arm_confirm(uint32_t token);

// Hard kill — return to SAFE immediately. Always permitted.
void lxveos_arm_disarm(void);

// THE gate: true iff TX is compiled in AND state is ARMED AND the inactivity timeout has not lapsed. Every
// offensive-TX op MUST call this immediately before transmitting; a true result refreshes the timeout.
bool lxveos_arm_can_emit(void);

#ifdef __cplusplus
}
#endif
