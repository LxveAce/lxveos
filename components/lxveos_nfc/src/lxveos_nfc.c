// LxveOS NFC PN532 driver — see lxveos_nfc.h. Increment 1: I2C bring-up, reader identify, read one card UID.
//
// PN532 normal-mode frame (host->reader): 00 00 FF LEN LCS TFI(D4) CMD PARAMS... DCS 00, where
// LEN = len(TFI..PARAMS), LCS = -LEN, DCS = -(sum TFI..PARAMS). Over I2C every read the reader returns first
// a ready-status byte (bit0=1 when a reply is waiting) then the frame; replies use TFI 0xD5 and CMD+1. This
// increment does SAMConfiguration + GetFirmwareVersion (identify) then InListPassiveTarget (read an
// ISO-14443A UID). Uses the ESP-IDF v6 i2c_master driver. UNVERIFIED on hardware; extra confirms the version
// read + a card read on a real PN532 module.
#include "lxveos_nfc.h"

#include <string.h>

#include "lxveos_arm.h"
#include "lxveos_radiomath.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *TAG = "lxveos_nfc";

#define PN532_I2C_ADDR 0x24
#define PN532_TFI_HOST 0xD4
#define PN532_TFI_RDR  0xD5

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool    s_begun;
static bool    s_present;
static uint8_t s_ic;
static uint8_t s_version;

// Build + send a command frame (cmd[0]=command byte, cmd[1..]=params). Frame bytes (preamble/LEN/LCS/
// TFI/DCS/postamble) come from lxveos_radiomath so the on-target path and the host test share one builder.
static esp_err_t pn532_send(const uint8_t *cmd, uint8_t clen)
{
    uint8_t f[48];
    size_t n = lxveos_pn532_build_frame(PN532_TFI_HOST, cmd, clen, f, sizeof(f));
    if (n == 0) {
        return ESP_ERR_INVALID_SIZE;   // command too long for the frame buffer
    }
    return i2c_master_transmit(s_dev, f, n, 100);
}

// Poll the 1-byte ready status until bit0 is set or the timeout lapses.
static bool pn532_wait_ready(uint32_t timeout_ms)
{
    uint8_t st = 0;
    for (uint32_t t = 0; t <= timeout_ms; t += 5) {
        if (i2c_master_receive(s_dev, &st, 1, 50) == ESP_OK && (st & 0x01)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

// Read + verify the 6-byte ACK (00 00 FF 00 FF 00), which follows the status byte.
static bool pn532_read_ack(void)
{
    uint8_t buf[7] = {0};
    if (i2c_master_receive(s_dev, buf, sizeof(buf), 100) != ESP_OK) {
        return false;
    }
    static const uint8_t ack[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    return memcmp(buf + 1, ack, sizeof(ack)) == 0;   // buf[0] = status byte
}

// Read a response frame; copy the payload (bytes after TFI + response-command) into data[]. Returns the
// payload length, or -1 on a malformed frame.
static int pn532_read_response(uint8_t *data, uint8_t data_cap)
{
    uint8_t buf[64] = {0};
    if (i2c_master_receive(s_dev, buf, sizeof(buf), 100) != ESP_OK) {
        return -1;
    }
    // buf[0] = status; frame = 00 00 FF LEN LCS TFI CMD+1 payload... DCS 00
    if (buf[1] != 0x00 || buf[2] != 0x00 || buf[3] != 0xFF) {
        return -1;
    }
    uint8_t len = buf[4];
    if ((uint8_t)(buf[4] + buf[5]) != 0) {   // LCS
        return -1;
    }
    if (buf[6] != PN532_TFI_RDR) {
        return -1;
    }
    int dlen = (int)len - 2;                  // minus TFI + response-command byte
    if (dlen < 0) {
        dlen = 0;
    }
    if (dlen > data_cap) {
        dlen = data_cap;
    }
    for (int k = 0; k < dlen; k++) {
        data[k] = buf[8 + k];
    }
    return dlen;
}

// Send a command, confirm the ACK, wait for the reply, read the payload. Returns payload length or -1.
static int pn532_transact(const uint8_t *cmd, uint8_t clen, uint8_t *data, uint8_t data_cap, uint32_t ready_ms)
{
    if (pn532_send(cmd, clen) != ESP_OK) {
        return -1;
    }
    if (!pn532_wait_ready(50) || !pn532_read_ack()) {
        return -1;
    }
    if (!pn532_wait_ready(ready_ms)) {
        return -1;
    }
    return pn532_read_response(data, data_cap);
}

esp_err_t lxveos_nfc_begin(int sda, int scl)
{
    if (s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sda < 0 || scl < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                       // auto-select a free I2C port
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PN532_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }
    s_begun = true;
    vTaskDelay(pdMS_TO_TICKS(10));            // PN532 wake-up settle

    // SAMConfiguration: normal mode, 1 s timeout, IRQ off.
    uint8_t sam[] = {0x14, 0x01, 0x14, 0x00};
    uint8_t rsp[16];
    pn532_transact(sam, sizeof(sam), rsp, sizeof(rsp), 100);

    // GetFirmwareVersion -> [IC, Ver, Rev, Support].
    uint8_t gfv[] = {0x02};
    int n = pn532_transact(gfv, sizeof(gfv), rsp, sizeof(rsp), 100);
    if (n >= 2) {
        s_ic = rsp[0];
        s_version = rsp[1];
        s_present = true;
    }
    ESP_LOGI(TAG, "PN532 begin: %s (IC=0x%02X ver=0x%02X)",
             s_present ? "detected" : "no response", s_ic, s_version);
    return ESP_OK;
}

esp_err_t lxveos_nfc_end(void)
{
    if (!s_begun) {
        return ESP_OK;
    }
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
    }
    s_dev = NULL;
    s_bus = NULL;
    s_begun = false;
    s_present = false;
    return ESP_OK;
}

uint8_t lxveos_nfc_version(void) { return s_version; }
uint8_t lxveos_nfc_ic(void)      { return s_ic; }
bool    lxveos_nfc_present(void) { return s_begun && s_present; }

esp_err_t lxveos_nfc_read_uid(uint32_t timeout_ms, uint8_t *uid, size_t uid_cap, size_t *uid_len,
                              uint8_t *sak, uint16_t *atqa)
{
    if (uid_len) {
        *uid_len = 0;
    }
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uid == NULL || uid_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = 5000;
    }

    // InListPassiveTarget: MaxTg=1, BrTy=0x00 (106 kbps ISO-14443A).
    uint8_t cmd[] = {0x4A, 0x01, 0x00};
    uint8_t rsp[32];
    int n = pn532_transact(cmd, sizeof(cmd), rsp, sizeof(rsp), timeout_ms);
    // rsp: [NbTg, Tg, ATQA_hi, ATQA_lo, SAK, UIDLen, UID...]
    if (n < 6 || rsp[0] < 1) {
        return ESP_ERR_TIMEOUT;      // no card presented
    }
    if (atqa) {
        *atqa = (uint16_t)((rsp[2] << 8) | rsp[3]);
    }
    if (sak) {
        *sak = rsp[4];
    }
    size_t ulen = rsp[5];
    if (ulen > uid_cap) {
        ulen = uid_cap;
    }
    if ((int)(6 + ulen) > n) {
        ulen = (n > 6) ? (size_t)(n - 6) : 0;
    }
    memcpy(uid, &rsp[6], ulen);
    if (uid_len) {
        *uid_len = ulen;
    }
    return ESP_OK;
}

esp_err_t lxveos_nfc_clone_write(const uint8_t *uid, size_t uid_len)
{
    if (!s_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uid == NULL || uid_len != 4) {
        return ESP_ERR_INVALID_ARG;
    }
    // Writing a spoofed UID to a card is an offensive-TX op: gate it on the two-factor arm, exactly like
    // subghz replay / nrf24 mousejack / badble. A single stray `nfc clone` cannot write a card while SAFE.
    if (!lxveos_arm_can_emit()) {
        return ESP_ERR_NOT_ALLOWED;
    }

    // 1) Select the presented (magic) card and learn its current UID (needed for the auth step).
    uint8_t cur[10];
    size_t cur_len = 0;
    esp_err_t e = lxveos_nfc_read_uid(3000, cur, sizeof(cur), &cur_len, NULL, NULL);
    if (e != ESP_OK) {
        return e;                          // no card presented
    }
    if (cur_len < 4) {
        return ESP_FAIL;
    }

    uint8_t rsp[16];
    // 2) InDataExchange: MifareAuthenticate block 0 with default key A + the card's current UID.
    uint8_t auth[14] = {0x40, 0x01, 0x60, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(&auth[10], cur, 4);
    int n = pn532_transact(auth, sizeof(auth), rsp, sizeof(rsp), 500);
    if (n < 1 || rsp[0] != 0x00) {
        return ESP_FAIL;                   // auth refused (wrong key / not writable)
    }

    // 3) Build block 0 = UID(4) + BCC + SAK(0x08) + ATQA(0x0004) + manufacturer(8), and write it.
    uint8_t bcc = lxveos_mifare_bcc4(uid);
    uint8_t wr[20] = {0x40, 0x01, 0xA0, 0x00,
                      uid[0], uid[1], uid[2], uid[3], bcc, 0x08, 0x04, 0x00,
                      0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69};
    n = pn532_transact(wr, sizeof(wr), rsp, sizeof(rsp), 500);
    if (n < 1 || rsp[0] != 0x00) {
        return ESP_FAIL;                   // write refused (not a Gen2 magic card)
    }
    ESP_LOGI(TAG, "cloned UID %02X%02X%02X%02X to block 0", uid[0], uid[1], uid[2], uid[3]);
    return ESP_OK;
}
