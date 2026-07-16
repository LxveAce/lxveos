// Host-side unit test for the arm-gate TX-safety state machine (components/lxveos_arm/src/lxveos_arm.c) — the
// single control between a stray command and offensive energy on-air. It is pure logic over file-static state
// plus esp_timer_get_time(), so it runs under a plain host compiler with a fake, settable clock (below) and
// tiny ESP-IDF stubs (stubs/). No ESP-IDF toolchain needed. A regression here (e.g. confirm() accepting a zero
// token, the confirm-window flipping, or can_emit() failing to auto-disarm) would silently defeat the gate;
// this test exists because a sibling offensive op (nfc_clone) was once shipped bypassing the gate entirely.
#include <stdint.h>
#include <stdio.h>

#include "lxveos_arm.h"

// The fake monotonic clock the esp_timer.h stub reads. Tests set it directly to drive the 30s confirm window
// and the 120s inactivity timeout deterministically.
static int64_t g_now_us = 0;
int64_t esp_timer_get_time(void) { return g_now_us; }

static int g_fail = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

static const int64_t SEC = 1000000;  // microseconds per second (esp_timer_get_time() units)

int main(void)
{
    if (!lxveos_arm_tx_compiled()) {
        // LXVEOS_TX_DISABLE build: the offensive emitter is stripped, so nothing can ever arm or emit.
        uint32_t tok = 0;
        CHECK(lxveos_arm_request(&tok) == ESP_ERR_NOT_SUPPORTED);
        CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);
        CHECK(lxveos_arm_can_emit() == false);
        printf(g_fail ? "arm host tests (TX_DISABLE): FAILED\n" : "arm host tests (TX_DISABLE): OK\n");
        return g_fail;
    }

    uint32_t tok = 0;

    // Fresh boot: SAFE, TX compiled in, cannot emit.
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);
    CHECK(lxveos_arm_can_emit() == false);

    // request -> PENDING with a nonzero one-time token; pending is not yet armed.
    g_now_us = 0;
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    CHECK(tok != 0);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_PENDING);
    CHECK(lxveos_arm_can_emit() == false);

    // Wrong token -> rejected, drops to SAFE (no retry on the same request).
    CHECK(lxveos_arm_confirm(tok ^ 1u) == ESP_ERR_INVALID_ARG);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);

    // A zero token is always rejected.
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    CHECK(lxveos_arm_confirm(0) == ESP_ERR_INVALID_ARG);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);

    // Confirm with no pending request -> INVALID_STATE.
    CHECK(lxveos_arm_confirm(1234) == ESP_ERR_INVALID_STATE);

    // Confirm after the 30s window has lapsed -> TIMEOUT, back to SAFE.
    g_now_us = 0;
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    g_now_us = 31 * SEC;
    CHECK(lxveos_arm_confirm(tok) == ESP_ERR_TIMEOUT);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);

    // Full arm inside the window -> ARMED, can emit.
    g_now_us = 100 * SEC;
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    g_now_us += 5 * SEC;  // 5s later, well inside the 30s confirm window
    CHECK(lxveos_arm_confirm(tok) == ESP_OK);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_ARMED);
    CHECK(lxveos_arm_can_emit() == true);

    // Activity refreshes the inactivity timer: emitting again just under 120s keeps it armed.
    g_now_us += 119 * SEC;
    CHECK(lxveos_arm_can_emit() == true);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_ARMED);

    // After 120s of inactivity, the gate auto-disarms and refuses.
    g_now_us += 121 * SEC;
    CHECK(lxveos_arm_can_emit() == false);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);

    // The advertised confirm window is the single source of truth AND matches the enforced timeout: a confirm
    // exactly at `window` seconds still succeeds, one second past it times out. Guards the wire value (window=)
    // and the human prose from ever drifting from CONFIRM_WINDOW_US.
    const uint32_t win = lxveos_arm_confirm_window_s();
    CHECK(win == 30);
    g_now_us = 1000 * SEC;
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    g_now_us += (int64_t)win * SEC;            // exactly `win` seconds later — still inside the window
    CHECK(lxveos_arm_confirm(tok) == ESP_OK);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_ARMED);
    lxveos_arm_disarm();
    g_now_us = 2000 * SEC;
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    g_now_us += (int64_t)(win + 1) * SEC;      // one second past the window -> timeout
    CHECK(lxveos_arm_confirm(tok) == ESP_ERR_TIMEOUT);
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);

    // Disarm is always available and idempotent.
    CHECK(lxveos_arm_request(&tok) == ESP_OK);
    lxveos_arm_disarm();
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);
    lxveos_arm_disarm();
    CHECK(lxveos_arm_state() == LXVEOS_ARM_SAFE);

    printf(g_fail ? "arm host tests: FAILED\n" : "arm host tests: OK\n");
    return g_fail;
}
