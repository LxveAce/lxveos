#pragma once
// Refcounted manager for the shared SPI3 (VSPI) bus. Two add-on radios live on SPI3 — the CC1101 (lxveos_subghz)
// and the nRF24 (lxveos_nrf24) — on the same operator-supplied SCLK/MISO/MOSI, kept off the display's SPI2.
// Before this, each driver called spi_bus_initialize()/spi_bus_free(SPI3_HOST) itself, so bringing up both and
// then ending one would free the bus out from under the other. This manager reference-counts the bus: the first
// acquire() initialises it, each further acquire() reuses it, and release() frees it only when the last holder
// lets go. Each driver still owns its own spi_bus_add_device()/remove for its CS line.
//
// The refcount POLICY (below) is a pure state machine, unit-tested off-target (tests/host_c/test_spibus.c);
// lxveos_spibus.c drives the actual spi_bus_* side effects from it.
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Acquire the shared SPI3 bus on the given GPIOs (initialise on the first hold, reuse + refcount otherwise).
// Returns ESP_OK, or an esp_err_t if the one-time spi_bus_initialize fails (in which case the refcount is
// NOT bumped). Pair every successful acquire with exactly one lxveos_spibus_release().
esp_err_t lxveos_spibus_acquire(int sclk, int miso, int mosi);

// Release one hold on the shared SPI3 bus; frees the bus only when the last holder releases. A release with
// no outstanding acquire is a no-op. Returns ESP_OK (or the esp_err_t from spi_bus_free on the final release).
esp_err_t lxveos_spibus_release(void);

// Current number of outstanding acquires (0 = bus down). For tests / introspection.
int lxveos_spibus_refcount(void);

// ── Pure refcount policy (host-testable; lxveos_spibus.c uses it to gate the spi_bus_* calls) ──────────
typedef enum {
    LXVEOS_SPIBUS_NONE = 0,   // reuse the existing bus / nothing physical to do
    LXVEOS_SPIBUS_INIT,       // this acquire must initialise the bus (refcount 0 -> 1)
    LXVEOS_SPIBUS_FREE,       // this release must free the bus (refcount 1 -> 0)
} lxveos_spibus_action_t;

// acquire: *next = cur + 1; returns INIT iff this is the first hold. A negative cur is treated as 0.
lxveos_spibus_action_t lxveos_spibus_acquire_action(int cur, int *next);

// release: *next = max(cur - 1, 0); returns FREE iff this dropped the last hold. cur <= 0 is a no-op (NONE).
lxveos_spibus_action_t lxveos_spibus_release_action(int cur, int *next);

#ifdef __cplusplus
}
#endif
