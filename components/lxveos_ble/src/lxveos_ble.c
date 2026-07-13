// LxveOS BLE recon (M1) — see lxveos_ble.h. A passive NimBLE observer.
//
// Flow: the first scan brings the NimBLE host up (nimble_port_init -> configure ble_hs_cfg callbacks ->
// ble_store_config_init -> run the host on its own FreeRTOS task) and waits for the controller/host to
// sync. Each scan then starts a PASSIVE ble_gap_disc and blocks the calling (REPL) task on a semaphore
// until the discovery-complete event fires; the GAP event callback — which runs on the NimBLE host task —
// fills the caller's device table in the meantime. The host stays up between scans (lazy, like Wi-Fi).
//
// STRICTLY PASSIVE: passive=1 means the controller never emits a SCAN_REQ, and the broadcaster/peripheral
// roles are compiled out entirely (sdkconfig.defaults), so this code cannot transmit even if it tried.
#include "lxveos_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "lxveos_ble";

// One-time NimBLE bring-up state. The host task and controller stay up after the first scan.
static bool             s_host_up;
static SemaphoreHandle_t s_sync_sem;   // given from on_sync once the host is ready to scan

// The in-flight scan's collection target. Lives on the caller's stack while it blocks on `done`; the GAP
// callback (host task) writes into it. Passed to ble_gap_disc as its cb_arg, so no global is needed for it.
struct scan_ctx {
    lxveos_ble_dev_t *out;
    size_t            max;
    size_t            count;
    SemaphoreHandle_t done;   // given from the callback on BLE_GAP_EVENT_DISC_COMPLETE
};

const char *lxveos_ble_addr_type_str(uint8_t addr_type)
{
    switch (addr_type) {
    case BLE_ADDR_PUBLIC:    return "public";
    case BLE_ADDR_RANDOM:    return "random";
    case BLE_ADDR_PUBLIC_ID: return "pub-id";
    case BLE_ADDR_RANDOM_ID: return "rnd-id";
    default:                 return "?";
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (reason %d)", reason);
}

static void on_sync(void)
{
    // Make sure the stack has a usable identity address (public from eFuse if present, else a generated
    // static-random one) before we ever try to scan.
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
    }
    if (s_sync_sem != NULL) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void nimble_host_task(void *param)
{
    (void)param;
    // Runs the NimBLE host event loop until nimble_port_stop() (which LxveOS never calls — the host is
    // meant to stay up for the device's lifetime). nimble_port_freertos_deinit() tidies up if it ever does.
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Bring the NimBLE host up once and block until it has synced (or a timeout). Idempotent: later calls that
// find the host already up return immediately.
static esp_err_t ensure_host_up(void)
{
    if (s_host_up) {
        return ESP_OK;
    }

    s_sync_sem = xSemaphoreCreateBinary();
    if (s_sync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();  // inits the BT controller + NimBLE host
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_sync_sem);
        s_sync_sem = NULL;
        return err;
    }

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE host did not sync within 5s");
        return ESP_ERR_TIMEOUT;
    }
    s_host_up = true;
    return ESP_OK;
}

// GAP event handler — runs on the NimBLE host task. Collects unique advertisers into the scan context and
// signals completion. Returns 0 (NimBLE ignores the value for discovery events).
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    struct scan_ctx *ctx = (struct scan_ctx *)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;

        // De-dup by (address, type): update an existing entry, else claim a new slot if room remains.
        lxveos_ble_dev_t *slot = NULL;
        for (size_t i = 0; i < ctx->count; i++) {
            if (ctx->out[i].addr_type == d->addr.type &&
                memcmp(ctx->out[i].addr, d->addr.val, 6) == 0) {
                slot = &ctx->out[i];
                break;
            }
        }
        if (slot == NULL) {
            if (ctx->count >= ctx->max) {
                return 0;  // table full — ignore further new devices (still purely passive)
            }
            slot = &ctx->out[ctx->count++];
            memset(slot, 0, sizeof(*slot));
            memcpy(slot->addr, d->addr.val, 6);
            slot->addr_type = d->addr.type;
        }
        slot->rssi = d->rssi;

        // Parse the advertising payload for GAP flags and a local name. Bounds are handled by NimBLE's
        // parser against length_data; we clamp the name to our buffer.
        if (d->length_data > 0) {
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0) {
                if (fields.flags_is_present) {
                    slot->adv_flags     = fields.flags;
                    slot->flags_present = true;
                }
                if (fields.name != NULL && fields.name_len > 0) {
                    uint8_t n = fields.name_len;
                    if (n > sizeof(slot->name) - 1) {
                        n = sizeof(slot->name) - 1;
                    }
                    memcpy(slot->name, fields.name, n);
                    slot->name[n]  = '\0';
                    slot->name_len = n;
                }
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (ctx->done != NULL) {
            xSemaphoreGive(ctx->done);
        }
        return 0;
    default:
        return 0;
    }
}

esp_err_t lxveos_ble_scan(uint32_t seconds, lxveos_ble_dev_t *out, size_t max, size_t *found)
{
    if (out == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (found != NULL) {
        *found = 0;
    }
    if (seconds == 0) {
        seconds = 6;
    }

    esp_err_t err = ensure_host_up();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return ESP_FAIL;
    }

    struct scan_ctx ctx = {.out = out, .max = max, .count = 0, .done = xSemaphoreCreateBinary()};
    if (ctx.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const struct ble_gap_disc_params disc = {
        .itvl              = 0,  // 0 -> NimBLE picks sensible default scan interval/window
        .window            = 0,
        .filter_policy     = 0,  // accept all advertisers
        .limited           = 0,
        .passive           = 1,  // PASSIVE: never emit SCAN_REQ — listen only
        .filter_duplicates = 0,  // we de-dup ourselves so RSSI/name keep refreshing
    };

    rc = ble_gap_disc(own_addr_type, (int32_t)(seconds * 1000), &disc, gap_event_cb, &ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        vSemaphoreDelete(ctx.done);
        return ESP_FAIL;
    }

    // Block until the controller reports discovery complete (+1.5s guard for the completion event).
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(seconds * 1000 + 1500));
    ble_gap_disc_cancel();  // no-op/EALREADY if it already stopped — just make sure the radio is idle

    vSemaphoreDelete(ctx.done);
    if (found != NULL) {
        *found = ctx.count;
    }
    return ESP_OK;
}
