// Host-side unit test for lxveos_spibus's pure refcount policy (the shared-SPI3 acquire/release state
// machine). Pure libc + the esp_err stub that lxveos_spibus.h pulls in — no ESP-IDF spi driver. Built + run
// by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_spibus.h"

#include <assert.h>
#include <stdio.h>

static void test_acquire_action(void)
{
    int next = -99;
    // First hold initialises the bus; further holds just reuse it.
    assert(lxveos_spibus_acquire_action(0, &next) == LXVEOS_SPIBUS_INIT && next == 1);
    assert(lxveos_spibus_acquire_action(1, &next) == LXVEOS_SPIBUS_NONE && next == 2);
    assert(lxveos_spibus_acquire_action(5, &next) == LXVEOS_SPIBUS_NONE && next == 6);
    // A corrupted negative count is treated as 0 so the next acquire still initialises (never skips init).
    assert(lxveos_spibus_acquire_action(-3, &next) == LXVEOS_SPIBUS_INIT && next == 1);
    // NULL next pointer must be safe.
    assert(lxveos_spibus_acquire_action(0, NULL) == LXVEOS_SPIBUS_INIT);
}

static void test_release_action(void)
{
    int next = -99;
    // Only the last release frees the bus.
    assert(lxveos_spibus_release_action(2, &next) == LXVEOS_SPIBUS_NONE && next == 1);
    assert(lxveos_spibus_release_action(1, &next) == LXVEOS_SPIBUS_FREE && next == 0);
    // Releasing with nothing held is a no-op — never a double free / negative count.
    assert(lxveos_spibus_release_action(0, &next) == LXVEOS_SPIBUS_NONE && next == 0);
    assert(lxveos_spibus_release_action(-1, &next) == LXVEOS_SPIBUS_NONE && next == 0);
    assert(lxveos_spibus_release_action(1, NULL) == LXVEOS_SPIBUS_FREE);
}

static void test_shared_lifecycle(void)
{
    // The exact bug this fixes: CC1101 + nRF24 both on SPI3. begin both, end one, end the other. The bus must
    // survive the first end and be freed only by the second.
    int c = 0, next;
    assert(lxveos_spibus_acquire_action(c, &next) == LXVEOS_SPIBUS_INIT); c = next;   // subghz.begin -> init
    assert(c == 1);
    assert(lxveos_spibus_acquire_action(c, &next) == LXVEOS_SPIBUS_NONE); c = next;   // nrf24.begin -> reuse
    assert(c == 2);
    assert(lxveos_spibus_release_action(c, &next) == LXVEOS_SPIBUS_NONE); c = next;   // subghz.end -> KEEP bus
    assert(c == 1);
    assert(lxveos_spibus_release_action(c, &next) == LXVEOS_SPIBUS_FREE); c = next;   // nrf24.end -> free
    assert(c == 0);
    // A stray extra release after teardown must not underflow or free again.
    assert(lxveos_spibus_release_action(c, &next) == LXVEOS_SPIBUS_NONE); c = next;
    assert(c == 0);
}

int main(void)
{
    test_acquire_action();
    test_release_action();
    test_shared_lifecycle();
    printf("test_spibus: all tests passed\n");
    return 0;
}
