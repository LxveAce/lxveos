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

#include <stdio.h>
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

// The NimBLE persistent-store bring-up. ESP-IDF's NimBLE port does not expose this in a public header
// (it lives in the store/config source), so — exactly as every ESP-IDF NimBLE example does — we declare
// its prototype here. It wires ble_hs_cfg's store callbacks to the RAM/NVS-backed default store.
void ble_store_config_init(void);

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

// A small table of Bluetooth SIG company identifiers — deliberately limited to vendors we are certain of
// and that matter for BLE-spam attribution (Apple/Microsoft/Google/Samsung are the popup-flood payloads),
// plus a couple of very common device vendors. Unknown IDs are shown as raw hex, never mis-attributed.
#define LXVEOS_BLE_CID_APPLE     0x004Cu
#define LXVEOS_BLE_CID_MICROSOFT 0x0006u
#define LXVEOS_BLE_CID_GOOGLE    0x00E0u
#define LXVEOS_BLE_CID_SAMSUNG   0x0075u
#define LXVEOS_BLE_SVC_FASTPAIR  0xFE2Cu  // Google Fast Pair service-data UUID

static const struct {
    uint16_t    id;
    const char *name;
} BLE_COMPANIES[] = {
    {LXVEOS_BLE_CID_APPLE,     "Apple"},
    {LXVEOS_BLE_CID_MICROSOFT, "Microsoft"},
    {LXVEOS_BLE_CID_GOOGLE,    "Google"},
    {LXVEOS_BLE_CID_SAMSUNG,   "Samsung"},
    {0x0059,                   "Nordic"},
    {0x0087,                   "Garmin"},
};

const char *lxveos_ble_company_name(uint16_t company_id)
{
    for (size_t i = 0; i < sizeof(BLE_COMPANIES) / sizeof(BLE_COMPANIES[0]); i++) {
        if (BLE_COMPANIES[i].id == company_id) {
            return BLE_COMPANIES[i].name;
        }
    }
    return NULL;
}

// A small table of common 16-bit BLE service-class UUIDs — the standard GATT services a device is likely to
// advertise (0x18xx, Bluetooth SIG assigned numbers) plus a few widely-deployed member service UUIDs
// (Fast Pair, Exposure Notification, Eddystone, Xiaomi). Certain-only; anything else is shown as raw hex.
static const struct {
    uint16_t    uuid;
    const char *name;
} BLE_SERVICES[] = {
    {0x1800, "GenAccess"},
    {0x1801, "GenAttr"},
    {0x180A, "DevInfo"},
    {0x180F, "Battery"},
    {0x180D, "HeartRate"},
    {0x1809, "Thermom"},
    {0x1810, "BloodPress"},
    {0x1812, "HID"},
    {0x1816, "CyclSpeed"},
    {0x1818, "CyclPower"},
    {0x1826, "FitMachine"},
    {LXVEOS_BLE_SVC_FASTPAIR, "FastPair"},  // 0xFE2C Google Fast Pair
    {0xFD6F,                  "ExpNotify"}, // Apple/Google Exposure Notification
    {0xFEAA,                  "Eddystone"}, // Google Eddystone beacon
    {0xFE95,                  "XiaomiMi"},  // Xiaomi Mijia
};

const char *lxveos_ble_service_name(uint16_t uuid16)
{
    for (size_t i = 0; i < sizeof(BLE_SERVICES) / sizeof(BLE_SERVICES[0]); i++) {
        if (BLE_SERVICES[i].uuid == uuid16) {
            return BLE_SERVICES[i].name;
        }
    }
    return NULL;
}

// GAP appearance category -> short label. Categories are the high 10 bits (value >> 6); the low 6 bits are
// a subcategory (only resolved for HID). Table follows the Bluetooth SIG assigned-numbers appearance list.
void lxveos_ble_appearance_str(uint16_t appearance, char *buf, size_t buflen)
{
    if (buflen == 0) {
        return;
    }
    uint16_t cat = (uint16_t)(appearance >> 6);
    if (cat == 15) {  // Human Interface Device — name the subcategory where we know it
        const char *hid;
        switch (appearance & 0x3F) {
        case 1:  hid = "Keyboard"; break;
        case 2:  hid = "Mouse";    break;
        case 3:  hid = "Joystick"; break;
        case 4:  hid = "Gamepad";  break;
        case 9:  hid = "Touchpad"; break;
        default: hid = "HID";      break;
        }
        snprintf(buf, buflen, "%s", hid);
        return;
    }
    const char *name = NULL;
    switch (cat) {
    case 1:  name = "Phone";      break;
    case 2:  name = "Computer";   break;
    case 3:  name = "Watch";      break;
    case 4:  name = "Clock";      break;
    case 5:  name = "Display";    break;
    case 6:  name = "Remote";     break;
    case 7:  name = "Glasses";    break;
    case 8:  name = "Tag";        break;
    case 10: name = "MediaPlyr";  break;
    case 12: name = "Thermom";    break;
    case 13: name = "HeartRate";  break;
    case 14: name = "BloodPress"; break;
    case 17: name = "Running";    break;
    case 18: name = "Cycling";    break;
    case 21: name = "Sensor";     break;
    case 33: name = "AudioSink";  break;  // speaker/headphone (audio receiver)
    case 34: name = "AudioSrc";   break;  // microphone/transmitter
    case 37: name = "Earbuds";    break;  // Wearable Audio Device
    case 41: name = "HearAid";    break;
    case 42: name = "Gaming";     break;
    case 50: name = "Scale";      break;
    default: break;
    }
    if (name != NULL) {
        snprintf(buf, buflen, "%s", name);
    } else {
        snprintf(buf, buflen, "appr:0x%04x", appearance);
    }
}

// Read the 16-bit manufacturer company ID from a parsed advert (mfg_data[0..1], little-endian); returns
// false if the advert carried no manufacturer-specific data.
static bool adv_company_id(const struct ble_hs_adv_fields *f, uint16_t *out_id)
{
    if (f->mfg_data == NULL || f->mfg_data_len < 2) {
        return false;
    }
    *out_id = (uint16_t)(f->mfg_data[0] | ((uint16_t)f->mfg_data[1] << 8));
    return true;
}

// True if the advert carries Google Fast Pair service data (16-bit service-data UUID 0xFE2C).
static bool adv_is_fastpair(const struct ble_hs_adv_fields *f)
{
    if (f->svc_data_uuid16 == NULL || f->svc_data_uuid16_len < 2) {
        return false;
    }
    uint16_t uuid = (uint16_t)(f->svc_data_uuid16[0] | ((uint16_t)f->svc_data_uuid16[1] << 8));
    return uuid == LXVEOS_BLE_SVC_FASTPAIR;
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
                // ble_hs_adv_fields has no presence bit for the flags octet (unlike tx-power/appearance);
                // the parser leaves flags==0 when the AD had no Flags field. Treat a non-zero octet as
                // "present" — good enough for a recon display, and 0 shows as "-".
                if (fields.flags != 0) {
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
                uint16_t cid;
                if (adv_company_id(&fields, &cid)) {
                    slot->company_id = cid;
                    slot->has_mfg    = true;
                }
                if (adv_is_fastpair(&fields)) {
                    slot->fastpair = true;
                }
                if (fields.appearance_is_present) {
                    slot->appearance         = fields.appearance;
                    slot->appearance_present = true;
                }
                // 16-bit service-class UUIDs (AD type 0x02 incomplete / 0x03 complete). NimBLE gives them
                // as an array of ble_uuid16_t; copy the .value of each into our fixed slot, flagging when
                // the advert listed more than we can hold. Refresh (don't accumulate) so a repeat sighting
                // reflects the latest advert.
                if (fields.uuids16 != NULL && fields.num_uuids16 > 0) {
                    const uint8_t cap = (uint8_t)(sizeof(slot->svc_uuids) / sizeof(slot->svc_uuids[0]));
                    uint8_t n = fields.num_uuids16;
                    slot->svc_uuids_partial = (n > cap);
                    if (n > cap) {
                        n = cap;
                    }
                    for (uint8_t u = 0; u < n; u++) {
                        slot->svc_uuids[u] = fields.uuids16[u].value;
                    }
                    slot->svc_uuid_count = n;
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

// ── BLE advertisement-flood / spam detection ────────────────────────────────────────────────────
//
// Same passive plumbing as lxveos_ble_scan, but instead of a device table it measures advertiser CHURN:
// how many DISTINCT addresses advertise in the window, how many are random-type, and which one is busiest.
// A BLE-spam / advert-flood attack rotates through a huge number of (usually random) addresses, so a high
// distinct-address count — or overflowing the tracking table — is the fingerprint.

#define LXVEOS_BLE_FLOOD_CAP 256  // distinct advertisers tracked; overflowing this is itself a flood signal

struct flood_entry {
    uint8_t  addr[6];
    uint8_t  type;
    uint32_t count;
};

struct flood_ctx {
    struct flood_entry *seen;    // -> a static table (kept off the small REPL task stack)
    size_t              cap;
    size_t              n;
    bool                overflow;
    uint32_t            total_adv;
    uint32_t            random_uniques;
    size_t              top_idx;
    uint32_t            top_count;
    // Per-advert vendor tally (see lxveos_ble_flood_stats_t) — attributes a flood to a vendor.
    uint32_t            v_apple;
    uint32_t            v_microsoft;
    uint32_t            v_google;
    uint32_t            v_samsung;
    uint32_t            v_fastpair;
    uint32_t            v_other_mfg;
    SemaphoreHandle_t   done;
};

// Classify one advert's payload by vendor and bump the matching flood counter. Parses defensively; an
// advert with no manufacturer/service data simply contributes to none of the vendor counters.
static void flood_classify(struct flood_ctx *c, const struct ble_gap_disc_desc *d)
{
    if (d->length_data == 0) {
        return;
    }
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return;
    }
    uint16_t cid;
    if (adv_company_id(&f, &cid)) {
        switch (cid) {
        case LXVEOS_BLE_CID_APPLE:     c->v_apple++;     break;
        case LXVEOS_BLE_CID_MICROSOFT: c->v_microsoft++; break;
        case LXVEOS_BLE_CID_GOOGLE:    c->v_google++;    break;
        case LXVEOS_BLE_CID_SAMSUNG:   c->v_samsung++;   break;
        default:                       c->v_other_mfg++; break;
        }
    }
    if (adv_is_fastpair(&f)) {
        c->v_fastpair++;
    }
}

static int flood_event_cb(struct ble_gap_event *event, void *arg)
{
    struct flood_ctx *c = (struct flood_ctx *)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        c->total_adv++;
        flood_classify(c, d);

        for (size_t i = 0; i < c->n; i++) {
            if (c->seen[i].type == d->addr.type && memcmp(c->seen[i].addr, d->addr.val, 6) == 0) {
                c->seen[i].count++;
                if (c->seen[i].count > c->top_count) {
                    c->top_count = c->seen[i].count;
                    c->top_idx   = i;
                }
                return 0;
            }
        }
        if (c->n >= c->cap) {
            c->overflow = true;  // ran out of table — the churn itself is the alarm
            return 0;
        }
        struct flood_entry *e = &c->seen[c->n];
        memcpy(e->addr, d->addr.val, 6);
        e->type  = d->addr.type;
        e->count = 1;
        if (d->addr.type == BLE_ADDR_RANDOM || d->addr.type == BLE_ADDR_RANDOM_ID) {
            c->random_uniques++;
        }
        if (c->top_count == 0) {
            c->top_count = 1;
            c->top_idx   = c->n;
        }
        c->n++;
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (c->done != NULL) {
            xSemaphoreGive(c->done);
        }
        return 0;
    default:
        return 0;
    }
}

esp_err_t lxveos_ble_flood_watch(uint32_t seconds, lxveos_ble_flood_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (seconds == 0) {
        seconds = 8;
    }
    out->seconds = seconds;

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

    // The distinct-address table is static so it never lands on the small REPL task stack. Scans are
    // serialized (one REPL command at a time), so a single shared table is safe; n=0 logically clears it.
    static struct flood_entry seen[LXVEOS_BLE_FLOOD_CAP];
    struct flood_ctx c = {.seen = seen, .cap = LXVEOS_BLE_FLOOD_CAP, .done = xSemaphoreCreateBinary()};
    if (c.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const struct ble_gap_disc_params disc = {
        .itvl              = 0,
        .window            = 0,
        .filter_policy     = 0,
        .limited           = 0,
        .passive           = 1,  // PASSIVE: listen only
        .filter_duplicates = 0,  // we WANT repeats — churn + per-address counts depend on them
    };

    rc = ble_gap_disc(own_addr_type, (int32_t)(seconds * 1000), &disc, flood_event_cb, &c);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        vSemaphoreDelete(c.done);
        return ESP_FAIL;
    }

    xSemaphoreTake(c.done, pdMS_TO_TICKS(seconds * 1000 + 1500));
    ble_gap_disc_cancel();
    vSemaphoreDelete(c.done);

    out->total_adv       = c.total_adv;
    out->unique_addrs    = (uint32_t)c.n;
    out->unique_overflow = c.overflow;
    out->random_addrs    = c.random_uniques;
    if (c.top_count > 0) {
        memcpy(out->top_addr, c.seen[c.top_idx].addr, 6);
        out->top_addr_type = c.seen[c.top_idx].type;
        out->top_count     = c.top_count;
    }
    out->v_apple     = c.v_apple;
    out->v_microsoft = c.v_microsoft;
    out->v_google    = c.v_google;
    out->v_samsung   = c.v_samsung;
    out->v_fastpair  = c.v_fastpair;
    out->v_other_mfg = c.v_other_mfg;
    return ESP_OK;
}
