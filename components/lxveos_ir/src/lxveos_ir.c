// LxveOS IR capture + replay — see lxveos_ir.h. Uses the ESP-IDF v6 RMT driver (esp_driver_rmt).
//
// Capture: an RMT RX channel on the receiver GPIO, 1 MHz resolution (1 µs/tick). rmt_receive() runs
// asynchronously; the on_recv_done callback (ISR context) hands the symbol block to a queue, and the
// caller blocks on that queue up to the timeout. The captured symbols are copied into a module-static
// buffer so a later replay can re-emit them.
//
// Replay: an RMT TX channel on the LED GPIO with a 38 kHz carrier and a copy encoder that streams the
// stored symbols out once. Channels are created per operation and torn down after, so RX and TX never hold
// a GPIO at the same time.
#include "lxveos_ir.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "lxveos_ir";

#define IR_RESOLUTION_HZ 1000000   // 1 MHz -> 1 tick = 1 µs
#define IR_MEM_SYMBOLS   64        // RMT hardware block (valid on every target, incl. ESP32-classic)
#define IR_MAX_SYMBOLS   128       // stored-capture capacity (a standard remote frame is well under this)
#define IR_CARRIER_HZ    38000     // standard consumer-IR carrier

// The last capture, retained for replay.
static rmt_symbol_word_t s_symbols[IR_MAX_SYMBOLS];
static size_t            s_count;

static bool valid_gpio(int gpio)
{
    return gpio >= 0 && gpio < 48;   // covers ESP32 / S3 GPIO ranges; RMT itself rejects an unroutable pin
}

// ISR-context RX-done callback: forward the event data to the waiting task via a queue.
static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *user)
{
    (void)chan;
    QueueHandle_t q = (QueueHandle_t)user;
    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(q, edata, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

esp_err_t lxveos_ir_capture(int rx_gpio, uint32_t timeout_ms, lxveos_ir_capture_info_t *info)
{
    if (info != NULL) {
        info->symbols = 0;
        info->truncated = false;
    }
    if (!valid_gpio(rx_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = 8000;
    }

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_MEM_SYMBOLS,
        .gpio_num        = rx_gpio,
    };
    rmt_channel_handle_t rx_chan = NULL;
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel(gpio %d) failed: %s", rx_gpio, esp_err_to_name(err));
        return err;
    }

    QueueHandle_t q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (q == NULL) {
        rmt_del_channel(rx_chan);
        return ESP_ERR_NO_MEM;
    }
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    err = rmt_rx_register_event_callbacks(rx_chan, &cbs, q);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = rmt_enable(rx_chan);
    if (err != ESP_OK) {
        goto cleanup;
    }

    // Accept pulses from 1.25 µs up to a 12 ms gap (ends the frame) — the standard consumer-IR window.
    rmt_receive_config_t recv_cfg = {
        .signal_range_min_ns = 1250,
        .signal_range_max_ns = 12000000,
    };
    rmt_symbol_word_t buf[IR_MAX_SYMBOLS];
    err = rmt_receive(rx_chan, buf, sizeof(buf), &recv_cfg);
    if (err != ESP_OK) {
        rmt_disable(rx_chan);
        goto cleanup;
    }

    rmt_rx_done_event_data_t rx_data;
    if (xQueueReceive(q, &rx_data, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        rmt_disable(rx_chan);
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    size_t n = rx_data.num_symbols;
    bool truncated = false;
    if (n > IR_MAX_SYMBOLS) {
        n = IR_MAX_SYMBOLS;
        truncated = true;
    }
    memcpy(s_symbols, rx_data.received_symbols, n * sizeof(rmt_symbol_word_t));
    s_count = n;
    if (info != NULL) {
        info->symbols = (uint32_t)n;
        info->truncated = truncated;
    }
    ESP_LOGI(TAG, "captured %u IR symbols%s", (unsigned)n, truncated ? " (truncated)" : "");

    rmt_disable(rx_chan);
    err = ESP_OK;

cleanup:
    rmt_del_channel(rx_chan);
    vQueueDelete(q);
    return err;
}

esp_err_t lxveos_ir_replay(int tx_gpio)
{
    if (!valid_gpio(tx_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_MEM_SYMBOLS,
        .trans_queue_depth = 4,
        .gpio_num          = tx_gpio,
    };
    rmt_channel_handle_t tx_chan = NULL;
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel(gpio %d) failed: %s", tx_gpio, esp_err_to_name(err));
        return err;
    }

    rmt_carrier_config_t carrier = {
        .duty_cycle   = 0.33,
        .frequency_hz = IR_CARRIER_HZ,
    };
    err = rmt_apply_carrier(tx_chan, &carrier);
    if (err != ESP_OK) {
        goto cleanup;
    }

    rmt_copy_encoder_config_t copy_cfg = {};   // empty config struct (C23 empty init; -std=gnu23)
    rmt_encoder_handle_t enc = NULL;
    err = rmt_new_copy_encoder(&copy_cfg, &enc);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = rmt_enable(tx_chan);
    if (err != ESP_OK) {
        rmt_del_encoder(enc);
        goto cleanup;
    }

    rmt_transmit_config_t tx_tcfg = { .loop_count = 0 };
    err = rmt_transmit(tx_chan, enc, s_symbols, s_count * sizeof(rmt_symbol_word_t), &tx_tcfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(tx_chan, pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "replayed %u IR symbols on gpio %d (%s)", (unsigned)s_count, tx_gpio, esp_err_to_name(err));

    rmt_disable(tx_chan);
    rmt_del_encoder(enc);

cleanup:
    rmt_del_channel(tx_chan);
    return err;
}

bool lxveos_ir_have_capture(void)
{
    return s_count > 0;
}

uint32_t lxveos_ir_capture_symbols(void)
{
    return (uint32_t)s_count;
}
