// LxveOS nRF24L01+ driver — see lxveos_nrf24.h. Increment 1: SPI bring-up, identify, RPD channel scan.
//
// SPI mode 0, MSB-first. Commands: R_REGISTER = 0x00|reg, W_REGISTER = 0x20|reg (5-bit reg address). CE is
// a separate GPIO (not the SPI CS): driving it high puts the radio into active RX/TX; a channel-energy
// sample is "CE high -> wait -> CE low -> read RPD". The RPD (Received Power Detector, reg 0x09 bit0) latches
// high when RX power exceeded ~-64 dBm during the listen — sweeping it across the 126 channels gives a
// 2.4 GHz activity map. UNVERIFIED on hardware; extra confirms the identity read + scan on a real module.
#include "lxveos_nrf24.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "lxveos_nrf24";

// nRF24 SPI commands / registers
#define NRF_CMD_R_REGISTER 0x00
#define NRF_CMD_W_REGISTER 0x20
#define NRF_CMD_FLUSH_RX   0xE2
#define NRF_REG_CONFIG     0x00
#define NRF_REG_EN_AA      0x01
#define NRF_REG_EN_RXADDR  0x02
#define NRF_REG_SETUP_AW   0x03
#define NRF_REG_RF_CH      0x05
#define NRF_REG_RF_SETUP   0x06
#define NRF_REG_STATUS     0x07
#define NRF_REG_RPD        0x09

static spi_device_handle_t s_dev;
static int  s_ce = -1;
static bool s_begun;
static bool s_present;

static uint8_t nrf_read_reg(uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(NRF_CMD_R_REGISTER | (reg & 0x1F)), 0xFF};
    uint8_t rx[2] = {0, 0};
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_dev, &t);
    return rx[1];
}

static void nrf_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F)), val};
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = NULL };
    spi_device_polling_transmit(s_dev, &t);
}

static void nrf_cmd(uint8_t cmd)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd, .rx_buffer = NULL };
    spi_device_polling_transmit(s_dev, &t);
}

static inline void ce(int level)
{
    if (s_ce >= 0) {
        gpio_set_level(s_ce, level);
    }
}

esp_err_t lxveos_nrf24_begin(int sck, int miso, int mosi, int csn, int ce_gpio)
{
    if (s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sck < 0 || miso < 0 || mosi < 0 || csn < 0 || ce_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = bus already up; reuse it
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4000000,   // 4 MHz — well within the nRF24's 10 MHz SPI limit
        .mode = 0,
        .spics_io_num = csn,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI3_HOST, &devcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI3_HOST);
        return err;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ce_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_ce = ce_gpio;
    ce(0);
    s_begun = true;

    vTaskDelay(pdMS_TO_TICKS(5));   // power-on settle

    // Configure as a primary receiver with auto-ack off (so it hears everything), then probe presence by
    // writing a test channel and reading it back.
    nrf_write_reg(NRF_REG_CONFIG, 0x03);   // PWR_UP | PRIM_RX
    nrf_write_reg(NRF_REG_EN_AA, 0x00);    // disable auto-ack on all pipes
    nrf_write_reg(NRF_REG_EN_RXADDR, 0x01);
    nrf_write_reg(NRF_REG_SETUP_AW, 0x03); // 5-byte address width
    nrf_write_reg(NRF_REG_RF_SETUP, 0x0F); // 2 Mbps, max PA (RX side; affects sensitivity only)
    nrf_cmd(NRF_CMD_FLUSH_RX);
    nrf_write_reg(NRF_REG_STATUS, 0x70);   // clear any latched IRQ flags

    nrf_write_reg(NRF_REG_RF_CH, 0x55);
    s_present = (nrf_read_reg(NRF_REG_RF_CH) == 0x55);
    ESP_LOGI(TAG, "nRF24 begin: RF_CH read-back %s", s_present ? "OK (module detected)" : "MISMATCH (check wiring)");
    return ESP_OK;
}

esp_err_t lxveos_nrf24_end(void)
{
    if (!s_begun) {
        return ESP_OK;
    }
    ce(0);
    nrf_write_reg(NRF_REG_CONFIG, 0x00);   // power down
    spi_bus_remove_device(s_dev);
    spi_bus_free(SPI3_HOST);
    if (s_ce >= 0) {
        gpio_reset_pin(s_ce);
    }
    s_dev = NULL;
    s_ce = -1;
    s_begun = false;
    s_present = false;
    return ESP_OK;
}

bool lxveos_nrf24_present(void)
{
    return s_begun && s_present;
}

esp_err_t lxveos_nrf24_scan(uint8_t *counts, uint16_t sweeps)
{
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (counts == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sweeps == 0) {
        sweeps = 100;
    }
    if (sweeps > 1000) {
        sweeps = 1000;
    }

    for (int i = 0; i < LXVEOS_NRF24_CHANNELS; i++) {
        counts[i] = 0;
    }

    for (uint16_t s = 0; s < sweeps; s++) {
        for (uint8_t ch = 0; ch < LXVEOS_NRF24_CHANNELS; ch++) {
            nrf_write_reg(NRF_REG_RF_CH, ch);
            ce(1);
            esp_rom_delay_us(200);      // listen window (RPD needs >~40 us of RX)
            ce(0);
            if (nrf_read_reg(NRF_REG_RPD) & 0x01) {
                if (counts[ch] < 255) {
                    counts[ch]++;
                }
            }
        }
        if ((s & 0x0F) == 0) {
            vTaskDelay(1);              // yield periodically so the watchdog stays happy
        }
    }
    return ESP_OK;
}
