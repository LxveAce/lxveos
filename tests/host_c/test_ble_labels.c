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

static void test_tracker_latch(void)
{
    // First sighting: NONE latch + a positive detection sets it.
    assert(lxveos_ble_tracker_latch(LXVEOS_BLE_TRACKER_NONE, LXVEOS_BLE_TRACKER_APPLE_FINDMY) ==
           LXVEOS_BLE_TRACKER_APPLE_FINDMY);
    // The bug this guards: a later signature-less advert (classifies NONE) must NOT erase the detection.
    assert(lxveos_ble_tracker_latch(LXVEOS_BLE_TRACKER_APPLE_FINDMY, LXVEOS_BLE_TRACKER_NONE) ==
           LXVEOS_BLE_TRACKER_APPLE_FINDMY);
    // A fresh positive sighting supersedes (re-detect / different signature wins over the stale one).
    assert(lxveos_ble_tracker_latch(LXVEOS_BLE_TRACKER_APPLE_FINDMY, LXVEOS_BLE_TRACKER_TILE) ==
           LXVEOS_BLE_TRACKER_TILE);
    // NONE over NONE stays NONE (a plain device is never spuriously flagged).
    assert(lxveos_ble_tracker_latch(LXVEOS_BLE_TRACKER_NONE, LXVEOS_BLE_TRACKER_NONE) ==
           LXVEOS_BLE_TRACKER_NONE);
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

static void test_classify_tracker(void)
{
    // Apple Find My: Apple company ID (4c 00, little-endian) + the Offline-Finding type byte 0x12.
    const uint8_t apple_fmn[] = {0x4c, 0x00, 0x12, 0xaa};
    assert(lxveos_ble_classify_tracker(apple_fmn, sizeof(apple_fmn), NULL, 0, NULL, 0)
           == LXVEOS_BLE_TRACKER_APPLE_FINDMY);
    // plain Apple with a non-Find-My type byte must NOT be flagged (the negative the type byte guards)
    const uint8_t apple_other[] = {0x4c, 0x00, 0x10};
    assert(lxveos_ble_classify_tracker(apple_other, sizeof(apple_other), NULL, 0, NULL, 0)
           == LXVEOS_BLE_TRACKER_NONE);
    // a non-Apple company ID that happens to carry 0x12 as its third byte -> NONE
    const uint8_t not_apple[] = {0x99, 0x00, 0x12};
    assert(lxveos_ble_classify_tracker(not_apple, sizeof(not_apple), NULL, 0, NULL, 0)
           == LXVEOS_BLE_TRACKER_NONE);

    // service-UUID trackers, advertised in the 16-bit UUID list (wire values, not the impl's constants)
    const uint16_t tile[] = {0xFEED};
    assert(lxveos_ble_classify_tracker(NULL, 0, tile, 1, NULL, 0) == LXVEOS_BLE_TRACKER_TILE);
    const uint16_t smarttag[] = {0x1234, 0xFD5A};
    assert(lxveos_ble_classify_tracker(NULL, 0, smarttag, 2, NULL, 0) == LXVEOS_BLE_TRACKER_SMARTTAG);
    const uint16_t chipolo[] = {0xFE33};
    assert(lxveos_ble_classify_tracker(NULL, 0, chipolo, 1, NULL, 0) == LXVEOS_BLE_TRACKER_CHIPOLO);
    const uint16_t pebblebee[] = {0xFA25};
    assert(lxveos_ble_classify_tracker(NULL, 0, pebblebee, 1, NULL, 0) == LXVEOS_BLE_TRACKER_PEBBLEBEE);

    // a tracker UUID carried in the service DATA (not the UUID list) is also recognised (both surfaces)
    const uint8_t tile_svcdata[] = {0xED, 0xFE};  // 0xFEED, little-endian
    assert(lxveos_ble_classify_tracker(NULL, 0, NULL, 0, tile_svcdata, sizeof(tile_svcdata))
           == LXVEOS_BLE_TRACKER_TILE);

    // Google Find My Network: 0xFEAA service DATA with frame byte 0x40
    const uint8_t fmn[] = {0xaa, 0xfe, 0x40, 0x01};
    assert(lxveos_ble_classify_tracker(NULL, 0, NULL, 0, fmn, sizeof(fmn)) == LXVEOS_BLE_TRACKER_GOOGLE_FMN);
    // a plain Eddystone beacon shares 0xFEAA but uses a different frame byte -> NONE (the key negative)
    const uint8_t eddystone_uid[] = {0xaa, 0xfe, 0x00, 0x01};
    assert(lxveos_ble_classify_tracker(NULL, 0, NULL, 0, eddystone_uid, sizeof(eddystone_uid))
           == LXVEOS_BLE_TRACKER_NONE);
    const uint8_t eddystone_url[] = {0xaa, 0xfe, 0x10, 0x01};
    assert(lxveos_ble_classify_tracker(NULL, 0, NULL, 0, eddystone_url, sizeof(eddystone_url))
           == LXVEOS_BLE_TRACKER_NONE);

    // nothing matching, and short/empty buffers never over-read
    assert(lxveos_ble_classify_tracker(NULL, 0, NULL, 0, NULL, 0) == LXVEOS_BLE_TRACKER_NONE);
    const uint8_t short_mfg[] = {0x4c, 0x00};  // len 2 (< 3): no Apple type byte to read
    assert(lxveos_ble_classify_tracker(short_mfg, sizeof(short_mfg), NULL, 0, NULL, 0)
           == LXVEOS_BLE_TRACKER_NONE);
    const uint8_t fmn_short[] = {0xaa, 0xfe};  // len 2 (< 3): can't confirm the FMN frame byte
    assert(lxveos_ble_classify_tracker(NULL, 0, NULL, 0, fmn_short, sizeof(fmn_short))
           == LXVEOS_BLE_TRACKER_NONE);
    const uint16_t unrelated[] = {0x1234, 0x5678};
    assert(lxveos_ble_classify_tracker(NULL, 0, unrelated, 2, NULL, 0) == LXVEOS_BLE_TRACKER_NONE);
}

static void test_decode_ibeacon(void)
{
    lxveos_ble_ibeacon_t b;
    // A well-formed Apple iBeacon: company 4c 00 (LE), type 0x02, length 0x15, 16B UUID, major/minor
    // big-endian, then the signed measured-power byte. 25 bytes total.
    const uint8_t ib[] = {
        0x4c, 0x00, 0x02, 0x15,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,  // proximity UUID
        0x12, 0x34,   // major (big-endian) = 0x1234
        0x56, 0x78,   // minor (big-endian) = 0x5678
        0xc5,         // measured power = -59 dBm (signed)
    };
    const uint8_t want_uuid[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                   0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    assert(lxveos_ble_decode_ibeacon(ib, sizeof(ib), &b) == true);
    assert(memcmp(b.uuid, want_uuid, 16) == 0);
    assert(b.major == 0x1234);
    assert(b.minor == 0x5678);
    assert(b.tx_power == -59);

    // Trailing bytes after the 25-byte frame are tolerated (real adverts may pad the AD element).
    uint8_t ib_pad[sizeof(ib) + 3];
    memcpy(ib_pad, ib, sizeof(ib));
    ib_pad[sizeof(ib)] = 0xff;
    ib_pad[sizeof(ib) + 1] = 0xee;
    ib_pad[sizeof(ib) + 2] = 0xdd;
    assert(lxveos_ble_decode_ibeacon(ib_pad, sizeof(ib_pad), &b) == true);
    assert(b.major == 0x1234);

    // Big-endian is honoured, not byte-swapped: 00 01 -> 1, 01 00 -> 256.
    uint8_t ord[sizeof(ib)];
    memcpy(ord, ib, sizeof(ib));
    ord[20] = 0x00; ord[21] = 0x01;   // major
    ord[22] = 0x01; ord[23] = 0x00;   // minor
    assert(lxveos_ble_decode_ibeacon(ord, sizeof(ord), &b) == true);
    assert(b.major == 1);
    assert(b.minor == 256);

    // Wrong company ID (not Apple) -> not an iBeacon.
    uint8_t not_apple[sizeof(ib)];
    memcpy(not_apple, ib, sizeof(ib));
    not_apple[0] = 0x99;
    not_apple[1] = 0x00;
    assert(lxveos_ble_decode_ibeacon(not_apple, sizeof(not_apple), &b) == false);

    // Apple but a Find My type byte (0x12), not iBeacon (0x02) -> false (keeps iBeacon and Find My distinct).
    uint8_t findmy[sizeof(ib)];
    memcpy(findmy, ib, sizeof(ib));
    findmy[2] = 0x12;
    assert(lxveos_ble_decode_ibeacon(findmy, sizeof(findmy), &b) == false);

    // Apple + type 0x02 but the wrong length byte -> false (a real iBeacon length is always 0x15).
    uint8_t badlen[sizeof(ib)];
    memcpy(badlen, ib, sizeof(ib));
    badlen[3] = 0x14;
    assert(lxveos_ble_decode_ibeacon(badlen, sizeof(badlen), &b) == false);

    // Short buffer (< 25) returns false and never reads past the given length.
    assert(lxveos_ble_decode_ibeacon(ib, 24, &b) == false);
    const uint8_t hdr_only[] = {0x4c, 0x00, 0x02, 0x15};
    assert(lxveos_ble_decode_ibeacon(hdr_only, sizeof(hdr_only), &b) == false);

    // NULL arguments are safe.
    assert(lxveos_ble_decode_ibeacon(NULL, 25, &b) == false);
    assert(lxveos_ble_decode_ibeacon(ib, sizeof(ib), NULL) == false);
}

static void test_decode_eddystone(void)
{
    lxveos_ble_eddystone_t e;

    // UID frame: [aa fe] uuid, [00] frame, [ec] tx=-20, 10B namespace, 6B instance.
    const uint8_t uid[] = {
        0xaa, 0xfe, 0x00, 0xec,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,  // namespace (10)
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,                          // instance (6)
    };
    const uint8_t want_ns[10] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a};
    const uint8_t want_inst[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    assert(lxveos_ble_decode_eddystone(uid, sizeof(uid), &e) == true);
    assert(e.frame_type == LXVEOS_BLE_EDDYSTONE_UID);
    assert(e.tx_power == -20);
    assert(memcmp(e.namespace_id, want_ns, 10) == 0);
    assert(memcmp(e.instance_id, want_inst, 6) == 0);

    // URL frame: scheme 0x00 (http://www.) + "google" + 0x00 (.com/) => http://www.google.com/
    const uint8_t url[] = {0xaa, 0xfe, 0x10, 0xf6, 0x00, 'g', 'o', 'o', 'g', 'l', 'e', 0x00};
    assert(lxveos_ble_decode_eddystone(url, sizeof(url), &e) == true);
    assert(e.frame_type == LXVEOS_BLE_EDDYSTONE_URL);
    assert(e.tx_power == -10);  // 0xf6
    assert(strcmp(e.url, "http://www.google.com/") == 0);

    // URL truncation: scheme 0x03 (https://) + 30 'a' literals overflows url[32] -> truncated + NUL-terminated.
    uint8_t longurl[5 + 30];
    longurl[0] = 0xaa; longurl[1] = 0xfe; longurl[2] = 0x10; longurl[3] = 0x00; longurl[4] = 0x03;
    for (int i = 0; i < 30; i++) {
        longurl[5 + i] = 'a';
    }
    assert(lxveos_ble_decode_eddystone(longurl, sizeof(longurl), &e) == true);
    assert(strlen(e.url) == 31);                        // exactly buflen-1, NUL-terminated, no overflow
    assert(strncmp(e.url, "https://aaaaaaaaaaaaaaaaaaaaaaa", 31) == 0);

    // TLM frame: version 0x00, vbatt 3000 mV, temp 29.0C (0x1d00), adv count 100, uptime 3600 (BE throughout).
    const uint8_t tlm[] = {
        0xaa, 0xfe, 0x20, 0x00,
        0x0b, 0xb8,               // vbatt = 0x0bb8 = 3000
        0x1d, 0x00,               // temp  = 0x1d00 = 7424 -> 29.0 C
        0x00, 0x00, 0x00, 0x64,   // adv count = 100
        0x00, 0x00, 0x0e, 0x10,   // uptime = 3600 (0.1 s units)
    };
    assert(lxveos_ble_decode_eddystone(tlm, sizeof(tlm), &e) == true);
    assert(e.frame_type == LXVEOS_BLE_EDDYSTONE_TLM);
    assert(e.vbatt_mv == 3000);
    assert(e.temp_c_256 == 7424);   // /256 = 29.0 C
    assert(e.adv_count == 100);
    assert(e.uptime_ds == 3600);

    // TLM negative temperature: 0xff00 -> -256 -> -1.0 C (signed 8.8 honoured).
    uint8_t tlm_neg[sizeof(tlm)];
    memcpy(tlm_neg, tlm, sizeof(tlm));
    tlm_neg[6] = 0xff; tlm_neg[7] = 0x00;
    assert(lxveos_ble_decode_eddystone(tlm_neg, sizeof(tlm_neg), &e) == true);
    assert(e.temp_c_256 == -256);

    // EID frame: recognized; only frame_type + tx exposed (the 8-byte ephemeral ID is not decoded).
    const uint8_t eid[] = {0xaa, 0xfe, 0x30, 0x0a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    assert(lxveos_ble_decode_eddystone(eid, sizeof(eid), &e) == true);
    assert(e.frame_type == LXVEOS_BLE_EDDYSTONE_EID);
    assert(e.tx_power == 10);

    // Wrong service UUID (not 0xFEAA) -> not Eddystone.
    uint8_t wrong_uuid[sizeof(uid)];
    memcpy(wrong_uuid, uid, sizeof(uid));
    wrong_uuid[0] = 0xab;   // 0xFEAB
    assert(lxveos_ble_decode_eddystone(wrong_uuid, sizeof(wrong_uuid), &e) == false);

    // The Find My Network frame (0x40) shares 0xFEAA but is a tracker, NOT Eddystone -> false (the key negative).
    const uint8_t fmn[] = {0xaa, 0xfe, 0x40, 0x01, 0x02, 0x03};
    assert(lxveos_ble_decode_eddystone(fmn, sizeof(fmn), &e) == false);
    // An unknown/reserved frame byte -> false.
    const uint8_t unknown[] = {0xaa, 0xfe, 0x50, 0x01, 0x02, 0x03};
    assert(lxveos_ble_decode_eddystone(unknown, sizeof(unknown), &e) == false);

    // Short buffers per frame type -> false, no over-read.
    assert(lxveos_ble_decode_eddystone(uid, 19, &e) == false);   // UID needs 20
    assert(lxveos_ble_decode_eddystone(tlm, 15, &e) == false);   // TLM needs 16
    const uint8_t url_short[] = {0xaa, 0xfe, 0x10, 0x00};        // URL needs the scheme byte at [4]
    assert(lxveos_ble_decode_eddystone(url_short, sizeof(url_short), &e) == false);
    const uint8_t two[] = {0xaa, 0xfe};                          // < 3: can't even read the frame byte
    assert(lxveos_ble_decode_eddystone(two, sizeof(two), &e) == false);

    // A URL frame with only the scheme (no body) decodes to just the scheme string.
    const uint8_t url_scheme_only[] = {0xaa, 0xfe, 0x10, 0x00, 0x02};  // scheme 0x02 = http://
    assert(lxveos_ble_decode_eddystone(url_scheme_only, sizeof(url_scheme_only), &e) == true);
    assert(strcmp(e.url, "http://") == 0);

    // NULL arguments are safe.
    assert(lxveos_ble_decode_eddystone(NULL, 20, &e) == false);
    assert(lxveos_ble_decode_eddystone(uid, sizeof(uid), NULL) == false);
}

int main(void)
{
    test_company_name();
    test_service_name();
    test_tracker_str();
    test_tracker_latch();
    test_classify_tracker();
    test_decode_ibeacon();
    test_decode_eddystone();
    test_appearance_str();
    test_flipper_color();
    test_meta();
    test_skimmer();
    test_flock();
    test_surveil();
    printf("test_ble_labels: all tests passed\n");
    return 0;
}
