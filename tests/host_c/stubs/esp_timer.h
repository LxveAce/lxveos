#pragma once
// Host-test stub for esp_timer.h — declares only the monotonic-clock reader lxveos_arm.c uses. The test
// provides the definition with a fake, settable clock so timeouts can be exercised deterministically.
#include <stdint.h>

int64_t esp_timer_get_time(void);
