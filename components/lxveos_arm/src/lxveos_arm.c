// LxveOS arm / safety framework (see lxveos_arm.h). The transmit gate for offensive ops.
//
// LxveOS is a licensed-lab tool, so offensive TX is compiled IN by default; the runtime two-factor arm +
// inactivity timeout below are the working safety — a single stray command cannot put energy on-air, and a
// walked-away-from unit disarms itself. A conservative / public build can define LXVEOS_TX_DISABLE to strip
// the offensive emitter out entirely, making lxveos_arm_can_emit() a hard false regardless of arm state.
#include "lxveos_arm.h"

#include <stddef.h>  // NULL — don't rely on it arriving transitively through the esp_* headers

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lxveos_arm";

#define CONFIRM_WINDOW_US (30LL * 1000 * 1000)    // 30s to echo the confirm token back
#define ARM_TIMEOUT_US    (120LL * 1000 * 1000)   // 120s of inactivity auto-disarms

static lxveos_arm_state_t s_state = LXVEOS_ARM_SAFE;
static uint32_t s_token;        // one-time confirm token for the pending request (0 = none)
static int64_t  s_pending_us;   // when the arm request was made
static int64_t  s_armed_us;     // last activity while armed (drives the inactivity timeout)

bool lxveos_arm_tx_compiled(void)
{
#if defined(LXVEOS_TX_DISABLE)
    return false;  // conservative / public build: offensive emitter stripped out
#else
    return true;   // default (licensed-lab tool): offensive TX built in, gated by the runtime arm
#endif
}

lxveos_arm_state_t lxveos_arm_state(void)
{
    return s_state;
}

const char *lxveos_arm_state_name(lxveos_arm_state_t s)
{
    switch (s) {
    case LXVEOS_ARM_SAFE:    return "safe";
    case LXVEOS_ARM_PENDING: return "pending";
    case LXVEOS_ARM_ARMED:   return "armed";
    default:                 return "?";
    }
}

esp_err_t lxveos_arm_request(uint32_t *token)
{
    if (!lxveos_arm_tx_compiled()) {
        return ESP_ERR_NOT_SUPPORTED;  // offensive TX compiled out — nothing to arm
    }
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now = esp_timer_get_time();
    // One-time confirm token derived from the boot-relative microsecond clock, forced nonzero. This is an
    // operator-in-the-loop confirmation (so one stray command can't arm), not a cryptographic secret.
    uint32_t t = (uint32_t)(now ^ (now >> 17)) | 1u;
    s_token = t;
    s_pending_us = now;
    s_state = LXVEOS_ARM_PENDING;
    *token = t;
    ESP_LOGW(TAG, "arm requested — confirm within 30s");
    return ESP_OK;
}

esp_err_t lxveos_arm_confirm(uint32_t token)
{
    if (s_state != LXVEOS_ARM_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_pending_us > CONFIRM_WINDOW_US) {
        s_state = LXVEOS_ARM_SAFE;
        s_token = 0;
        return ESP_ERR_TIMEOUT;
    }
    if (token == 0 || token != s_token) {
        s_state = LXVEOS_ARM_SAFE;  // a wrong token drops to SAFE — no retries on the same request
        s_token = 0;
        return ESP_ERR_INVALID_ARG;
    }
    s_token = 0;
    s_armed_us = now;
    s_state = LXVEOS_ARM_ARMED;
    ESP_LOGW(TAG, "ARMED — offensive TX permitted until disarm/timeout");
    return ESP_OK;
}

void lxveos_arm_disarm(void)
{
    if (s_state != LXVEOS_ARM_SAFE) {
        ESP_LOGW(TAG, "disarmed");
    }
    s_state = LXVEOS_ARM_SAFE;
    s_token = 0;
}

bool lxveos_arm_can_emit(void)
{
    if (!lxveos_arm_tx_compiled() || s_state != LXVEOS_ARM_ARMED) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_armed_us > ARM_TIMEOUT_US) {
        lxveos_arm_disarm();
        return false;
    }
    s_armed_us = now;  // activity refreshes the inactivity timer
    return true;
}
