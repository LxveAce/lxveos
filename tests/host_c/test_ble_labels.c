// Host-side unit test for lxveos_ble_labels (BLE value->label helpers). Pure libc + the esp_err stub that
// lxveos_ble.h pulls in — no NimBLE. Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first
// failed assertion.
#include "lxveos_ble.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_company_name(void)
{
    assert(strcmp(lxveos_ble_company_name(0x004C), "Apple") == 0);
    assert(strcmp(lxveos_ble_company_name(0x0006), "Microsoft") == 0);
    assert(strcmp(lxveos_ble_company_name(0x00E0), "Google") == 0);
    assert(strcmp(lxveos_ble_company_name(0x0075), "Samsung") == 0);
    assert(strcmp(lxveos_ble_company_name(0x0059), "Nordic") == 0);
    assert(strcmp(lxveos_ble_company_name(0x0087), "Garmin") == 0);
    // Unknown company -> NULL (caller shows raw hex; never mis-attributed).
    assert(lxveos_ble_company_name(0x1234) == NULL);
    assert(lxveos_ble_company_name(0x0000) == NULL);
}

static void test_service_name(void)
{
    assert(strcmp(lxveos_ble_service_name(0x1800), "GenAccess") == 0);
    assert(strcmp(lxveos_ble_service_name(0x180F), "Battery") == 0);
    assert(strcmp(lxveos_ble_service_name(0x180D), "HeartRate") == 0);
    assert(strcmp(lxveos_ble_service_name(0x1812), "HID") == 0);
    assert(strcmp(lxveos_ble_service_name(0xFE2C), "FastPair") == 0);
    assert(strcmp(lxveos_ble_service_name(0xFEAA), "Eddystone") == 0);
    assert(strcmp(lxveos_ble_service_name(0xFE95), "XiaomiMi") == 0);
    // Unknown UUID -> NULL.
    assert(lxveos_ble_service_name(0x9999) == NULL);
}

static void test_tracker_str(void)
{
    assert(strcmp(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_APPLE_FINDMY), "AirTag/FindMy") == 0);
    assert(strcmp(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_TILE), "Tile") == 0);
    assert(strcmp(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_SMARTTAG), "SmartTag") == 0);
    assert(strcmp(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_CHIPOLO), "Chipolo") == 0);
    assert(strcmp(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_PEBBLEBEE), "PebbleBee") == 0);
    assert(strcmp(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_GOOGLE_FMN), "GoogleFMN") == 0);
    // NONE and any unknown classification -> NULL (the "not a tracker" honesty gate).
    assert(lxveos_ble_tracker_str(LXVEOS_BLE_TRACKER_NONE) == NULL);
    assert(lxveos_ble_tracker_str(99) == NULL);
}

static void test_appearance_str(void)
{
    char buf[32];
    // HID category (cat 15 = value >> 6) resolves its keyboard/mouse subcategory.
    lxveos_ble_appearance_str(0x03C1, buf, sizeof(buf)); assert(strcmp(buf, "Keyboard") == 0);  // 15<<6 | 1
    lxveos_ble_appearance_str(0x03C2, buf, sizeof(buf)); assert(strcmp(buf, "Mouse") == 0);     // 15<<6 | 2
    lxveos_ble_appearance_str(0x03C0, buf, sizeof(buf)); assert(strcmp(buf, "HID") == 0);       // 15<<6 | 0
    // Named consumer categories.
    lxveos_ble_appearance_str(64,  buf, sizeof(buf)); assert(strcmp(buf, "Phone") == 0);   // cat 1
    lxveos_ble_appearance_str(192, buf, sizeof(buf)); assert(strcmp(buf, "Watch") == 0);   // cat 3
    lxveos_ble_appearance_str(33 * 64, buf, sizeof(buf)); assert(strcmp(buf, "AudioSink") == 0);
    // Unknown category -> raw hex "appr:0x....", never mis-labelled.
    lxveos_ble_appearance_str(0x0000, buf, sizeof(buf)); assert(strcmp(buf, "appr:0x0000") == 0); // cat 0
    lxveos_ble_appearance_str(99 * 64, buf, sizeof(buf)); assert(strcmp(buf, "appr:0x18c0") == 0);
    // buflen == 0 must write nothing (no out-of-bounds).
    buf[0] = 'X';
    lxveos_ble_appearance_str(64, buf, 0);
    assert(buf[0] == 'X');
}

static void test_flipper_color(void)
{
    lxveos_ble_dev_t d = {0};
    // A Flipper advertises one of 0x3081/0x3082/0x3083 among its service UUIDs -> case colour.
    d.svc_uuids[0] = 0x180f;   // Battery — an unrelated standard UUID alongside it
    d.svc_uuids[1] = 0x3082;
    d.svc_uuid_count = 2;
    assert(strcmp(lxveos_ble_flipper_color(&d), "White") == 0);
    d.svc_uuids[0] = 0x3081;
    d.svc_uuid_count = 1;
    assert(strcmp(lxveos_ble_flipper_color(&d), "Black") == 0);
    d.svc_uuids[0] = 0x3083;
    assert(strcmp(lxveos_ble_flipper_color(&d), "Transparent") == 0);

    // A non-Flipper advertiser (only standard UUIDs) is not matched.
    lxveos_ble_dev_t n = {0};
    n.svc_uuids[0] = 0x180f;
    n.svc_uuids[1] = 0x1812;   // HID
    n.svc_uuid_count = 2;
    assert(lxveos_ble_flipper_color(&n) == NULL);

    // count bounds the match: a Flipper UUID sitting past svc_uuid_count is ignored (stale slot).
    lxveos_ble_dev_t m = {0};
    m.svc_uuids[0] = 0x3081;
    m.svc_uuid_count = 0;
    assert(lxveos_ble_flipper_color(&m) == NULL);

    // NULL is safe.
    assert(lxveos_ble_flipper_color(NULL) == NULL);
}

int main(void)
{
    test_company_name();
    test_service_name();
    test_tracker_str();
    test_appearance_str();
    test_flipper_color();
    printf("test_ble_labels: all tests passed\n");
    return 0;
}
