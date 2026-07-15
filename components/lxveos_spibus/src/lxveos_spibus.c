// lxveos_spibus — refcounted shared SPI3 bus manager (see lxveos_spibus.h). Holds the one static refcount and
// turns the pure policy decisions (lxveos_spibus_policy.c) into the actual spi_bus_initialize()/spi_bus_free()
// calls, so the CC1101 and nRF24 add-ons can share SPI3 without one freeing it under the other. Not
// thread-safe against itself: callers (the subghz / nrf24 begin/end paths) run from the single console task.
#include "lxveos_spibus.h"

#include "esp_log.h"
#include "driver/spi_master.h"

static const char *TAG = "lxveos_spibus";

#define LXVEOS_SPIBUS_HOST SPI3_HOST   // VSPI on ESP32 — kept off the display's SPI2/HSPI

static int s_refcount;

esp_err_t lxveos_spibus_acquire(int sclk, int miso, int mosi)
{
    int next = 0;
    if (lxveos_spibus_acquire_action(s_refcount, &next) == LXVEOS_SPIBUS_INIT) {
        spi_bus_config_t bus = {
            .mosi_io_num = mosi,
            .miso_io_num = miso,
            .sclk_io_num = sclk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 64,
        };
        esp_err_t err = spi_bus_initialize(LXVEOS_SPIBUS_HOST, &bus, SPI_DMA_DISABLED);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = already up; reuse it
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
            return err;   // leave the refcount untouched — the acquire did not succeed
        }
    }
    s_refcount = next;
    return ESP_OK;
}

esp_err_t lxveos_spibus_release(void)
{
    int next = 0;
    esp_err_t err = ESP_OK;
    if (lxveos_spibus_release_action(s_refcount, &next) == LXVEOS_SPIBUS_FREE) {
        err = spi_bus_free(LXVEOS_SPIBUS_HOST);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_free: %s", esp_err_to_name(err));
        }
    }
    s_refcount = next;
    return err;
}

int lxveos_spibus_refcount(void)
{
    return s_refcount;
}
