// lxveos_spibus_policy — the pure refcount state machine for the shared SPI3 bus manager (see
// lxveos_spibus.h). No ESP-IDF, no hardware: it just decides, from the current refcount and the requested
// operation, whether the physical bus must be initialised, freed, or left alone. lxveos_spibus.c calls these
// to gate its spi_bus_initialize()/spi_bus_free() calls; the host test drives them directly.
#include "lxveos_spibus.h"

#include <stddef.h>   // NULL

lxveos_spibus_action_t lxveos_spibus_acquire_action(int cur, int *next)
{
    if (cur < 0) {
        cur = 0;   // defensive: never let a corrupted count skip the init
    }
    if (next != NULL) {
        *next = cur + 1;
    }
    return (cur == 0) ? LXVEOS_SPIBUS_INIT : LXVEOS_SPIBUS_NONE;
}

lxveos_spibus_action_t lxveos_spibus_release_action(int cur, int *next)
{
    if (cur <= 0) {
        if (next != NULL) {
            *next = 0;
        }
        return LXVEOS_SPIBUS_NONE;   // nothing held -> nothing to free
    }
    int n = cur - 1;
    if (next != NULL) {
        *next = n;
    }
    return (n == 0) ? LXVEOS_SPIBUS_FREE : LXVEOS_SPIBUS_NONE;
}
