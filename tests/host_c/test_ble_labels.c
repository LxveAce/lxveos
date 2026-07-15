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

static void test_meta(void)
{
    // A device advertising a Meta company ID (mfg data) is Meta.
    lxveos_ble_dev_t d = {0};
    d.has_mfg = true;
    d.company_id = 0xFD5F;   // Oculus VR (Meta)
    assert(lxveos_ble_is_meta(&d) == true);

    // A Meta match can also come from an advertised service UUID.
    lxveos_ble_dev_t s = {0};
    s.svc_uuids[0] = 0x180f;  // Battery — unrelated standard UUID alongside it
    s.svc_uuids[1] = 0x0D53;  // Luxottica (Ray-Ban Meta)
    s.svc_uuid_count = 2;
    assert(lxveos_ble_is_meta(&s) == true);

    // Blocked wins: a device carrying BOTH a Meta ID and a deny-listed ID is NOT Meta (strips the
    // Apple/Samsung/Microsoft popup-flood payloads that would otherwise false-match).
    lxveos_ble_dev_t b = {0};
    b.has_mfg = true;
    b.company_id = 0xFD5F;    // Meta...
    b.svc_uuids[0] = 0xFD5A;  // ...but also a SmartTag block signal -> denied
    b.svc_uuid_count = 1;
    assert(lxveos_ble_is_meta(&b) == false);

    // A plain Apple advertiser (deny-listed, no Meta ID) is not Meta.
    lxveos_ble_dev_t a = {0};
    a.has_mfg = true;
    a.company_id = 0x004C;    // Apple
    assert(lxveos_ble_is_meta(&a) == false);

    // A device with no Meta identifier at all is not Meta.
    lxveos_ble_dev_t n = {0};
    n.svc_uuids[0] = 0x1812;  // HID
    n.svc_uuid_count = 1;
    assert(lxveos_ble_is_meta(&n) == false);

    // company_id is only a candidate when has_mfg is set (a stale company_id with has_mfg=false is ignored).
    lxveos_ble_dev_t stale = {0};
    stale.has_mfg = false;
    stale.company_id = 0xFD5F;
    assert(lxveos_ble_is_meta(&stale) == false);

    // Every Meta match ID is recognized on its own (guards against a typo in any one constant).
    static const uint16_t meta_ids[] = {0xFD5F, 0xFEB7, 0xFEB8, 0x01AB, 0x058E, 0x0D53};
    for (size_t i = 0; i < sizeof(meta_ids) / sizeof(meta_ids[0]); i++) {
        lxveos_ble_dev_t x = {0};
        x.has_mfg = true;
        x.company_id = meta_ids[i];
        assert(lxveos_ble_is_meta(&x) == true);
    }
    // Every deny ID blocks even a genuine Meta mfg match (guards the deny-list constants + blocked-wins order).
    static const uint16_t deny_ids[] = {0xFD5A, 0xFD69, 0x004C, 0x0006, 0xFEF3};
    for (size_t i = 0; i < sizeof(deny_ids) / sizeof(deny_ids[0]); i++) {
        lxveos_ble_dev_t x = {0};
        x.has_mfg = true;
        x.company_id = 0xFD5F;         // a real Meta id...
        x.svc_uuids[0] = deny_ids[i];  // ...but a deny id is present => blocked
        x.svc_uuid_count = 1;
        assert(lxveos_ble_is_meta(&x) == false);
    }

    // Service-DATA surface: a device advertising the Meta anchor 0xFD5F ONLY as service DATA is caught (the
    // gap the DEBUG pass found — 0xFD5F is a SIG member UUID real devices carry in service data, not mfg).
    lxveos_ble_dev_t sd = {0};
    sd.svc_data_uuid16 = 0xFD5F;
    assert(lxveos_ble_is_meta(&sd) == true);
    // ...and a deny id in the service-DATA surface blocks even a genuine mfg Meta match.
    lxveos_ble_dev_t sdb = {0};
    sdb.has_mfg = true;
    sdb.company_id = 0x0D53;        // Meta (Luxottica / Ray-Ban)
    sdb.svc_data_uuid16 = 0xFEF3;   // phone-popup deny id, in service DATA => blocked
    assert(lxveos_ble_is_meta(&sdb) == false);

    // NULL is safe.
    assert(lxveos_ble_is_meta(NULL) == false);
}

static void test_skimmer(void)
{
    // Exact default HC-0x BT-serial module names -> possible skimmer.
    lxveos_ble_dev_t d = {0};
    strcpy(d.name, "HC-05");
    d.name_len = 5;
    assert(lxveos_ble_is_skimmer(&d) == true);
    strcpy(d.name, "HC-06");
    assert(lxveos_ble_is_skimmer(&d) == true);
    strcpy(d.name, "HC-03");
    assert(lxveos_ble_is_skimmer(&d) == true);

    // EXACT match only — a renamed or differently-cased module is not flagged (narrow heuristic).
    lxveos_ble_dev_t r = {0};
    strcpy(r.name, "HC-05-BT");
    r.name_len = 8;
    assert(lxveos_ble_is_skimmer(&r) == false);
    strcpy(r.name, "hc-05");
    r.name_len = 5;
    assert(lxveos_ble_is_skimmer(&r) == false);

    // A device with no advertised name is not flagged.
    lxveos_ble_dev_t e = {0};
    e.name_len = 0;
    assert(lxveos_ble_is_skimmer(&e) == false);

    // NULL is safe.
    assert(lxveos_ble_is_skimmer(NULL) == false);
}

static void test_flock(void)
{
    // XUNTONG mfg + a confirming Flock name => LIKELY.
    lxveos_ble_dev_t p = {0};
    p.has_mfg = true;
    p.company_id = 0x09C8;   // XUNTONG
    strcpy(p.name, "Penguin-0123456789");
    p.name_len = 18;
    assert(lxveos_ble_flock_confidence(&p) == LXVEOS_BLE_FLOCK_LIKELY);
    assert(strcmp(lxveos_ble_flock_str(lxveos_ble_flock_confidence(&p)), "likely") == 0);

    // Legacy exact name.
    strcpy(p.name, "FS Ext Battery");
    p.name_len = 14;
    assert(lxveos_ble_flock_confidence(&p) == LXVEOS_BLE_FLOCK_LIKELY);

    // Newer firmware: bare 10-digit serial name.
    strcpy(p.name, "1234567890");
    p.name_len = 10;
    assert(lxveos_ble_flock_confidence(&p) == LXVEOS_BLE_FLOCK_LIKELY);

    // XUNTONG but nameless => POSSIBLE (weaker).
    lxveos_ble_dev_t q = {0};
    q.has_mfg = true;
    q.company_id = 0x09C8;
    q.name_len = 0;
    assert(lxveos_ble_flock_confidence(&q) == LXVEOS_BLE_FLOCK_POSSIBLE);
    assert(strcmp(lxveos_ble_flock_str(lxveos_ble_flock_confidence(&q)), "possible") == 0);

    // XUNTONG with some OTHER (non-Flock) name is NOT flagged (name-or-nameless gate).
    lxveos_ble_dev_t r = {0};
    r.has_mfg = true;
    r.company_id = 0x09C8;
    strcpy(r.name, "MyThermostat");
    r.name_len = 12;
    assert(lxveos_ble_flock_confidence(&r) == LXVEOS_BLE_FLOCK_NONE);

    // "Penguin-" with a non-digit in the serial does NOT match (exact pattern).
    strcpy(r.name, "Penguin-01234X6789");
    r.name_len = 18;
    assert(lxveos_ble_flock_confidence(&r) == LXVEOS_BLE_FLOCK_NONE);

    // Bare-10 branch: a 10-char ALL-ALPHA name is not a digit serial => XUNTONG device NOT flagged.
    strcpy(r.name, "ABCDEFGHIJ");
    r.name_len = 10;
    assert(lxveos_ble_flock_confidence(&r) == LXVEOS_BLE_FLOCK_NONE);

    // A Flock-looking NAME without the XUNTONG mfg ID is NOT flagged — the mfg ID is the required signal
    // (we don't carry the FP-prone name-only / OUI heuristics).
    lxveos_ble_dev_t n = {0};
    n.has_mfg = false;
    strcpy(n.name, "Penguin-0123456789");
    n.name_len = 18;
    assert(lxveos_ble_flock_confidence(&n) == LXVEOS_BLE_FLOCK_NONE);

    // A different mfg ID with a Flock name is not flagged either.
    lxveos_ble_dev_t m = {0};
    m.has_mfg = true;
    m.company_id = 0x004C;   // Apple, not XUNTONG
    strcpy(m.name, "FS Ext Battery");
    m.name_len = 14;
    assert(lxveos_ble_flock_confidence(&m) == LXVEOS_BLE_FLOCK_NONE);

    // NONE has no label; NULL is safe.
    assert(lxveos_ble_flock_str(LXVEOS_BLE_FLOCK_NONE) == NULL);
    assert(lxveos_ble_flock_confidence(NULL) == LXVEOS_BLE_FLOCK_NONE);
}

static void test_surveil(void)
{
    // A plain advertiser matches nothing.
    lxveos_ble_dev_t plain = {0};
    plain.svc_uuids[0] = 0x180f;  // Battery
    plain.svc_uuid_count = 1;
    assert(lxveos_ble_surveil_flags(&plain) == LXVEOS_SURVEIL_NONE);

    // An item-tracker (tracker field set by the scan classifier) -> TRACKER bit.
    lxveos_ble_dev_t t = {0};
    t.tracker = LXVEOS_BLE_TRACKER_APPLE_FINDMY;
    assert(lxveos_ble_surveil_flags(&t) == LXVEOS_SURVEIL_TRACKER);

    // A Flipper -> FLIPPER bit.
    lxveos_ble_dev_t fl = {0};
    fl.svc_uuids[0] = 0x3082;
    fl.svc_uuid_count = 1;
    assert(lxveos_ble_surveil_flags(&fl) == LXVEOS_SURVEIL_FLIPPER);

    // A Meta advertiser -> META bit.
    lxveos_ble_dev_t me = {0};
    me.has_mfg = true;
    me.company_id = 0xFD5F;
    assert(lxveos_ble_surveil_flags(&me) == LXVEOS_SURVEIL_META);

    // A Flock (XUNTONG + name) -> FLOCK bit.
    lxveos_ble_dev_t fc = {0};
    fc.has_mfg = true;
    fc.company_id = 0x09C8;
    strcpy(fc.name, "FS Ext Battery");
    fc.name_len = 14;
    assert(lxveos_ble_surveil_flags(&fc) == LXVEOS_SURVEIL_FLOCK);

    // A default-named BT-serial module -> SKIMMER bit.
    lxveos_ble_dev_t sk = {0};
    strcpy(sk.name, "HC-05");
    sk.name_len = 5;
    assert(lxveos_ble_surveil_flags(&sk) == LXVEOS_SURVEIL_SKIMMER);

    // Multi-category: a Meta advertiser that ALSO carries a Flipper service UUID sets BOTH bits (verifies the
    // "can match more than one bit" claim and the cmd_surveil "cat1+cat2" join).
    lxveos_ble_dev_t mc = {0};
    mc.has_mfg = true;
    mc.company_id = 0xFD5F;    // Meta
    mc.svc_uuids[0] = 0x3082;  // Flipper (White)
    mc.svc_uuid_count = 1;
    assert(lxveos_ble_surveil_flags(&mc) == (LXVEOS_SURVEIL_META | LXVEOS_SURVEIL_FLIPPER));

    // Category-bit labels (one bit per call), and 0 / unknown -> NULL.
    assert(strcmp(lxveos_ble_surveil_str(LXVEOS_SURVEIL_TRACKER), "tracker") == 0);
    assert(strcmp(lxveos_ble_surveil_str(LXVEOS_SURVEIL_FLOCK), "flock-cam") == 0);
    assert(strcmp(lxveos_ble_surveil_str(LXVEOS_SURVEIL_META), "meta-glasses") == 0);
    assert(strcmp(lxveos_ble_surveil_str(LXVEOS_SURVEIL_FLIPPER), "flipper") == 0);
    assert(strcmp(lxveos_ble_surveil_str(LXVEOS_SURVEIL_SKIMMER), "skimmer?") == 0);
    assert(lxveos_ble_surveil_str(LXVEOS_SURVEIL_NONE) == NULL);
    assert(lxveos_ble_surveil_str(0xFF) == NULL);

    // NULL is safe.
    assert(lxveos_ble_surveil_flags(NULL) == LXVEOS_SURVEIL_NONE);
}

int main(void)
{
    test_company_name();
    test_service_name();
    test_tracker_str();
    test_appearance_str();
    test_flipper_color();
    test_meta();
    test_skimmer();
    test_flock();
    test_surveil();
    printf("test_ble_labels: all tests passed\n");
    return 0;
}
