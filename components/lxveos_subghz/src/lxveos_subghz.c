// LxveOS sub-GHz CC1101 driver — see lxveos_subghz.h. Increment 1: SPI bring-up, chip identify, RSSI sense.
//
// The CC1101 speaks SPI mode 0, MSB-first. Every access starts with a header byte: address in the low 6
// bits, bit6 = burst, bit7 = read. Status registers (0x30-0x3D) alias the command strobes, so reading them
// requires the burst bit set (addr | 0xC0). Command strobes (SRES/SRX/SIDLE/…) are single header bytes.
//
// Register values below come from the widely-used community CC1101 base configuration (SmartRC/ELECHOUSE
// lineage) for 433.92 MHz ASK/OOK receive. UNVERIFIED on hardware — this compiles and is structurally
// correct; extra confirms the identity read + RSSI behaviour on a real module (that is why the catalog row
// stays planned until HW-validated).
#include "lxveos_subghz.h"
#include "lxveos_arm.h"
#include "lxveos_radiomath.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "lxveos_subghz";

// CC1101 command strobes
#define CC1101_SRES   0x30
#define CC1101_SRX    0x34
#define CC1101_STX    0x35
#define CC1101_SIDLE  0x36
#define CC1101_SFRX   0x3A
#define CC1101_SFTX   0x3B
// Status registers (read with the burst bit, addr|0xC0)
#define CC1101_PARTNUM 0x30
#define CC1101_VERSION 0x31
#define CC1101_RSSI    0x34
#define CC1101_MARCSTATE 0x35
// Config registers used here
#define CC1101_IOCFG0 0x02
#define CC1101_PKTCTRL0 0x08
#define CC1101_FREQ2  0x0D
#define CC1101_FREQ1  0x0E
#define CC1101_FREQ0  0x0F

#define CC1101_WRITE_BURST 0x40
#define CC1101_READ_SINGLE 0x80
#define CC1101_READ_BURST  0xC0

static spi_device_handle_t s_dev;
static bool    s_begun;
static uint8_t s_partnum;
static uint8_t s_version;

// One SPI transfer: send header (+optional data), return the byte clocked back on the last octet.
static uint8_t spi_xfer(uint8_t addr, uint8_t data, uint8_t *status_out)
{
    uint8_t tx[2] = {addr, data};
    uint8_t rx[2] = {0, 0};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) {
        if (status_out) *status_out = 0xFF;
        return 0;
    }
    if (status_out) *status_out = rx[0];   // chip status byte returned on the header
    return rx[1];
}

static void cc1101_strobe(uint8_t cmd)
{
    uint8_t tx = cmd, rx = 0;
    spi_transaction_t t = { .length = 8, .tx_buffer = &tx, .rx_buffer = &rx };
    spi_device_polling_transmit(s_dev, &t);
}

static void cc1101_write_reg(uint8_t addr, uint8_t value)
{
    spi_xfer(addr, value, NULL);
}

static uint8_t cc1101_read_reg(uint8_t addr, bool status_reg)
{
    uint8_t hdr = addr | (status_reg ? CC1101_READ_BURST : CC1101_READ_SINGLE);
    return spi_xfer(hdr, 0x00, NULL);
}

// Base 433.92 MHz ASK/OOK receive configuration (community CC1101 profile). Only the registers needed for
// a stable RX + RSSI reading are set; frequency is (re)programmed per scan in lxveos_subghz_rssi().
static void cc1101_load_base_config(void)
{
    static const uint8_t cfg[][2] = {
        {0x00, 0x2E}, // IOCFG2  - GDO2 high-Z
        {0x02, 0x06}, // IOCFG0  - assert on sync/EOP
        {0x03, 0x47}, // FIFOTHR
        {0x08, 0x05}, // PKTCTRL0 - RX, variable length, no CRC
        {0x0B, 0x06}, // FSCTRL1
        {0x0D, 0x10}, // FREQ2  (~433.92 MHz; overwritten per scan)
        {0x0E, 0xB0}, // FREQ1
        {0x0F, 0x71}, // FREQ0
        {0x10, 0x56}, // MDMCFG4 - RX bandwidth
        {0x11, 0x4C}, // MDMCFG3 - data rate
        {0x12, 0x30}, // MDMCFG2 - ASK/OOK, no preamble/sync
        {0x13, 0x22}, // MDMCFG1
        {0x14, 0xF8}, // MDMCFG0
        {0x15, 0x15}, // DEVIATN
        {0x18, 0x18}, // MCSM0 - auto-calibrate on IDLE->RX
        {0x19, 0x16}, // FOCCFG
        {0x1B, 0x43}, // AGCCTRL2
        {0x1C, 0x40}, // AGCCTRL1
        {0x1D, 0x91}, // AGCCTRL0
        {0x21, 0x56}, // FREND1
        {0x22, 0x11}, // FREND0
        {0x23, 0xE9}, // FSCAL3
        {0x24, 0x2A}, // FSCAL2
        {0x25, 0x00}, // FSCAL1
        {0x26, 0x1F}, // FSCAL0
        {0x2C, 0x81}, // TEST2
        {0x2D, 0x35}, // TEST1
        {0x2E, 0x09}, // TEST0
    };
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        cc1101_write_reg(cfg[i][0], cfg[i][1]);
    }
}

esp_err_t lxveos_subghz_begin(int sclk, int miso, int mosi, int cs)
{
    if (s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sclk < 0 || miso < 0 || mosi < 0 || cs < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = bus already up; reuse it
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,   // 1 MHz — conservative, well within CC1101 limits
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI3_HOST, &devcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI3_HOST);
        return err;
    }
    s_begun = true;

    // Reset sequence + base config, then read identity.
    cc1101_strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(5));
    cc1101_load_base_config();
    s_partnum = cc1101_read_reg(CC1101_PARTNUM, true);
    s_version = cc1101_read_reg(CC1101_VERSION, true);
    ESP_LOGI(TAG, "CC1101 begin: PARTNUM=0x%02X VERSION=0x%02X", s_partnum, s_version);
    return ESP_OK;
}

esp_err_t lxveos_subghz_end(void)
{
    if (!s_begun) {
        return ESP_OK;
    }
    cc1101_strobe(CC1101_SIDLE);
    spi_bus_remove_device(s_dev);
    spi_bus_free(SPI3_HOST);
    s_dev = NULL;
    s_begun = false;
    return ESP_OK;
}

bool lxveos_subghz_present(void)
{
    // CC1101 VERSION is 0x14 (some batches 0x04/0x17); reject the all-ones / all-zero bus-float readings.
    return s_begun && s_version != 0x00 && s_version != 0xFF;
}

uint8_t lxveos_subghz_partnum(void) { return s_partnum; }
uint8_t lxveos_subghz_version(void) { return s_version; }

esp_err_t lxveos_subghz_rssi(float mhz, int8_t *rssi_dbm)
{
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mhz < 300.0f || mhz > 928.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    // FREQ = f / (fXOSC / 2^16). Program FREQ2/1/0.
    uint32_t freq_word = lxveos_cc1101_freq_to_word(mhz);
    cc1101_strobe(CC1101_SIDLE);
    cc1101_write_reg(CC1101_FREQ2, (freq_word >> 16) & 0xFF);
    cc1101_write_reg(CC1101_FREQ1, (freq_word >> 8) & 0xFF);
    cc1101_write_reg(CC1101_FREQ0, freq_word & 0xFF);
    cc1101_strobe(CC1101_SRX);
    vTaskDelay(pdMS_TO_TICKS(5));   // let AGC settle

    uint8_t raw = cc1101_read_reg(CC1101_RSSI, true);
    // TI conversion: rssi_dBm = (raw>=128 ? (raw-256) : raw)/2 - 74 (RSSI_offset for 433 MHz).
    if (rssi_dbm) {
        *rssi_dbm = lxveos_cc1101_rssi_to_dbm(raw);
    }
    cc1101_strobe(CC1101_SIDLE);
    return ESP_OK;
}

// ── Increment 2: OOK capture + replay via CC1101 async-serial mode + RMT ──────────────────────────────
#define SG_RESOLUTION_HZ 1000000   // 1 MHz -> 1 us/tick
#define SG_MEM_SYMBOLS   64
#define SG_MAX_SYMBOLS   256

static rmt_symbol_word_t s_syms[SG_MAX_SYMBOLS];
static size_t            s_nsym;
static float             s_cap_mhz;

static void cc1101_set_freq(float mhz)
{
    uint32_t fw = lxveos_cc1101_freq_to_word(mhz);
    cc1101_write_reg(CC1101_FREQ2, (fw >> 16) & 0xFF);
    cc1101_write_reg(CC1101_FREQ1, (fw >> 8) & 0xFF);
    cc1101_write_reg(CC1101_FREQ0, fw & 0xFF);
}

static bool IRAM_ATTR sg_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *user)
{
    (void)ch;
    QueueHandle_t q = (QueueHandle_t)user;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(q, ed, &hp);
    return hp == pdTRUE;
}

esp_err_t lxveos_subghz_capture(int gdo0_gpio, float mhz, uint32_t timeout_ms, uint32_t *symbols)
{
    if (symbols) {
        *symbols = 0;
    }
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gdo0_gpio < 0 || mhz < 300.0f || mhz > 928.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = 8000;
    }

    // CC1101 -> async-serial RX: GDO0 outputs the demodulated OOK bitstream.
    cc1101_strobe(CC1101_SIDLE);
    cc1101_set_freq(mhz);
    cc1101_write_reg(CC1101_IOCFG0, 0x0D);    // GDO0 = async serial data output
    cc1101_write_reg(CC1101_PKTCTRL0, 0x30);  // async serial mode, infinite packet length
    cc1101_strobe(CC1101_SRX);

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SG_RESOLUTION_HZ,
        .mem_block_symbols = SG_MEM_SYMBOLS,
        .gpio_num = gdo0_gpio,
    };
    rmt_channel_handle_t rx = NULL;
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &rx);
    if (err != ESP_OK) {
        cc1101_strobe(CC1101_SIDLE);
        return err;
    }
    QueueHandle_t q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (q == NULL) {
        rmt_del_channel(rx);
        cc1101_strobe(CC1101_SIDLE);
        return ESP_ERR_NO_MEM;
    }
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = sg_rx_done };
    rmt_rx_register_event_callbacks(rx, &cbs, q);
    rmt_enable(rx);

    rmt_receive_config_t rc = { .signal_range_min_ns = 10000, .signal_range_max_ns = 20000000 };
    rmt_symbol_word_t buf[SG_MAX_SYMBOLS];
    err = rmt_receive(rx, buf, sizeof(buf), &rc);
    if (err == ESP_OK) {
        rmt_rx_done_event_data_t ev;
        if (xQueueReceive(q, &ev, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            size_t n = ev.num_symbols;
            if (n > SG_MAX_SYMBOLS) {
                n = SG_MAX_SYMBOLS;
            }
            memcpy(s_syms, ev.received_symbols, n * sizeof(rmt_symbol_word_t));
            s_nsym = n;
            s_cap_mhz = mhz;
            if (symbols) {
                *symbols = (uint32_t)n;
            }
            ESP_LOGI(TAG, "captured %u sub-GHz OOK symbols @ %.2f MHz", (unsigned)n, (double)mhz);
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }

    rmt_disable(rx);
    rmt_del_channel(rx);
    vQueueDelete(q);
    cc1101_strobe(CC1101_SIDLE);
    return err;
}

esp_err_t lxveos_subghz_replay(int gdo0_gpio)
{
    if (!s_begun || s_nsym == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gdo0_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lxveos_arm_can_emit()) {
        return ESP_ERR_NOT_ALLOWED;   // offensive TX — must be armed
    }

    // CC1101 -> async-serial TX: GDO0 is the data input; RMT drives it, the CC1101 does the RF modulation.
    cc1101_strobe(CC1101_SIDLE);
    cc1101_set_freq(s_cap_mhz);
    cc1101_write_reg(CC1101_IOCFG0, 0x2D);    // GDO0 = async serial data input
    cc1101_write_reg(CC1101_PKTCTRL0, 0x30);  // async serial mode
    cc1101_strobe(CC1101_STX);

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SG_RESOLUTION_HZ,
        .mem_block_symbols = SG_MEM_SYMBOLS,
        .trans_queue_depth = 4,
        .gpio_num = gdo0_gpio,
    };
    rmt_channel_handle_t tx = NULL;
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &tx);
    if (err != ESP_OK) {
        cc1101_strobe(CC1101_SIDLE);
        return err;
    }
    // NO RMT carrier — the CC1101 supplies the RF carrier; RMT only keys the OOK envelope.
    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_encoder_handle_t enc = NULL;
    err = rmt_new_copy_encoder(&copy_cfg, &enc);
    if (err == ESP_OK) {
        rmt_enable(tx);
        rmt_transmit_config_t tc = { .loop_count = 0 };
        err = rmt_transmit(tx, enc, s_syms, s_nsym * sizeof(rmt_symbol_word_t), &tc);
        if (err == ESP_OK) {
            err = rmt_tx_wait_all_done(tx, pdMS_TO_TICKS(2000));
        }
        rmt_disable(tx);
        rmt_del_encoder(enc);
    }
    ESP_LOGI(TAG, "replayed %u sub-GHz OOK symbols @ %.2f MHz (%s)",
             (unsigned)s_nsym, (double)s_cap_mhz, esp_err_to_name(err));

    rmt_del_channel(tx);
    cc1101_strobe(CC1101_SIDLE);
    return err;
}

bool lxveos_subghz_have_capture(void) { return s_nsym > 0; }
uint32_t lxveos_subghz_capture_symbols(void) { return (uint32_t)s_nsym; }
