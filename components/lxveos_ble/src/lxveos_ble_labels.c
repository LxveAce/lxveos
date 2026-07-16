// lxveos_ble_labels — pure BLE value->label helpers split out of lxveos_ble.c so they can be host-unit-
// tested (tests/host_c/test_ble_labels.c) without NimBLE: the company/service name lookups, the item-tracker
// label, and the GAP appearance label. The NimBLE-coupled bits (addr_type_str, the advert classifier and its
// ble_hs_adv_fields parsing) stay in lxveos_ble.c. Behaviour-preserving extraction — the tables and the
// mappings are unchanged, so scan/tracker output is identical.
#include "lxveos_ble.h"
#include "lxveos_ble_internal.h"

#include <stdio.h>
#include <string.h>

// A small table of Bluetooth SIG company identifiers — deliberately limited to vendors we are certain of
// and that matter for BLE-spam attribution (Apple/Microsoft/Google/Samsung are the popup-flood payloads),
// plus a couple of very common device vendors. Unknown IDs are shown as raw hex, never mis-attributed.
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

const char *lxveos_ble_tracker_str(uint8_t tracker)
{
    switch (tracker) {
    case LXVEOS_BLE_TRACKER_APPLE_FINDMY: return "AirTag/FindMy";
    case LXVEOS_BLE_TRACKER_TILE:         return "Tile";
    case LXVEOS_BLE_TRACKER_SMARTTAG:     return "SmartTag";
    case LXVEOS_BLE_TRACKER_CHIPOLO:      return "Chipolo";
    case LXVEOS_BLE_TRACKER_PEBBLEBEE:    return "PebbleBee";
    case LXVEOS_BLE_TRACKER_GOOGLE_FMN:   return "GoogleFMN";
    default:                              return NULL;
    }
}

uint8_t lxveos_ble_tracker_latch(uint8_t current, uint8_t sighting)
{
    // A passive scan can see several adverts from one address in a window (an ADV_IND carrying the tracker
    // signature, plus a passively-overheard SCAN_RSP that carries only a name). Classifying each sighting and
    // overwriting would let a later signature-less frame erase a real detection, so a positive detection is
    // latched: keep it unless this sighting is itself positive (a fresh positive supersedes, e.g. re-detect).
    return (sighting != LXVEOS_BLE_TRACKER_NONE) ? sighting : current;
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

const char *lxveos_ble_flipper_color(const lxveos_ble_dev_t *d)
{
    if (d == NULL) {
        return NULL;
    }
    // Match on the already-parsed 16-bit service UUIDs (AD type 0x02/0x03) — a proper AD-structure match, so
    // (unlike Marauder's raw-payload byte scan) an unrelated field that merely contains these bytes can't
    // false-match. svc_uuid_count is capped at the array size by the parser; bound defensively anyway.
    uint8_t n = d->svc_uuid_count;
    if (n > 8) {
        n = 8;
    }
    for (uint8_t i = 0; i < n; i++) {
        switch (d->svc_uuids[i]) {
        case LXVEOS_BLE_UUID_FLIPPER_BLACK:       return "Black";
        case LXVEOS_BLE_UUID_FLIPPER_WHITE:       return "White";
        case LXVEOS_BLE_UUID_FLIPPER_TRANSPARENT: return "Transparent";
        default: break;
        }
    }
    return NULL;
}

// The Meta match set + the deny-list that wins over it (see lxveos_ble_internal.h). Ported from ESP32 Marauder
// "Meta Detect". Kept SEPARATE from the item-tracker table on purpose: 0xFD5A is a SmartTag *tracker* signal
// there but a *block* signal here.
static const uint16_t META_IDS[] = {
    LXVEOS_BLE_META_ID_0, LXVEOS_BLE_META_ID_1, LXVEOS_BLE_META_ID_2,
    LXVEOS_BLE_META_ID_3, LXVEOS_BLE_META_ID_4, LXVEOS_BLE_META_ID_5,
};
static const uint16_t META_BLOCKED[] = {
    LXVEOS_BLE_SVC_SMARTTAG, LXVEOS_BLE_META_BLOCK_SAMSUNG2, LXVEOS_BLE_CID_APPLE,
    LXVEOS_BLE_CID_MICROSOFT, LXVEOS_BLE_META_BLOCK_PHONE,
};

static bool uint16_in(uint16_t v, const uint16_t *set, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (set[i] == v) {
            return true;
        }
    }
    return false;
}

bool lxveos_ble_is_meta(const lxveos_ble_dev_t *d)
{
    if (d == NULL) {
        return false;
    }
    // Candidate 16-bit identifiers = manufacturer company ID (if present) + advertised service-class UUIDs +
    // the first service-DATA UUID (AD 0x16). Covering all three surfaces matches Marauder and, importantly,
    // catches Meta's own SIG-verified anchor 0xFD5F, which real devices advertise as service DATA. Blocked
    // wins: any candidate in the deny-list => not Meta (so a service-data-only 0xFD5A/0xFEF3 also blocks).
    uint16_t cand[2 + 8];
    size_t nc = 0;
    if (d->has_mfg) {
        cand[nc++] = d->company_id;
    }
    if (d->svc_data_uuid16 != 0) {
        cand[nc++] = d->svc_data_uuid16;
    }
    uint8_t su = d->svc_uuid_count;
    if (su > 8) {
        su = 8;
    }
    for (uint8_t i = 0; i < su; i++) {
        cand[nc++] = d->svc_uuids[i];
    }
    for (size_t i = 0; i < nc; i++) {
        if (uint16_in(cand[i], META_BLOCKED, sizeof(META_BLOCKED) / sizeof(META_BLOCKED[0]))) {
            return false;
        }
    }
    for (size_t i = 0; i < nc; i++) {
        if (uint16_in(cand[i], META_IDS, sizeof(META_IDS) / sizeof(META_IDS[0]))) {
            return true;
        }
    }
    return false;
}

// Card-skimmer default BT-serial module names (ESP32 Marauder "Detect Card Skimmers"). EXACT full-name match —
// many gas-pump/ATM skimmers reuse an HC-0x SPP module with its stock name. NOTE: narrow heuristic — it also
// flags legit hobby HC-0x modules (present as "possible", never definitive) and, since LxveOS scans BLE only,
// won't see classic-BT-only modules. Faithful to what Marauder catches.
static const char *const SKIMMER_NAMES[] = {"HC-03", "HC-05", "HC-06"};

bool lxveos_ble_is_skimmer(const lxveos_ble_dev_t *d)
{
    if (d == NULL || d->name_len == 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(SKIMMER_NAMES) / sizeof(SKIMMER_NAMES[0]); i++) {
        if (strcmp(d->name, SKIMMER_NAMES[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Does `name` (NUL-terminated, name_len bytes) match one of Flock's confirming name patterns? (See
// lxveos_ble_internal.h.) All three come from Marauder's isFlockCamera(): the legacy exact name, the
// "Penguin-" + 10-digit serial, and the newer bare 10-all-digit serial. Pure over the name bytes.
static bool flock_name_match(const char *name, uint8_t name_len)
{
    // Legacy exact name.
    if (name_len == 14 && strcmp(name, "FS Ext Battery") == 0) {
        return true;
    }
    // "Penguin-" (8 chars) + exactly 10 digits => total length 18.
    if (name_len == 18 && strncmp(name, "Penguin-", 8) == 0) {
        for (int i = 8; i < 18; i++) {
            if (name[i] < '0' || name[i] > '9') {
                return false;
            }
        }
        return true;
    }
    // Newer firmware: the name is the bare serial — exactly 10 characters, all digits.
    if (name_len == 10) {
        for (int i = 0; i < 10; i++) {
            if (name[i] < '0' || name[i] > '9') {
                return false;
            }
        }
        return true;
    }
    return false;
}

uint8_t lxveos_ble_flock_confidence(const lxveos_ble_dev_t *d)
{
    if (d == NULL) {
        return LXVEOS_BLE_FLOCK_NONE;
    }
    // The XUNTONG manufacturer ID is the only Flock signal LxveOS trusts (the broad-OUI + SSID-substring paths
    // are too FP-prone to carry). Not XUNTONG => not Flock, full stop.
    if (!d->has_mfg || d->company_id != LXVEOS_BLE_CID_XUNTONG) {
        return LXVEOS_BLE_FLOCK_NONE;
    }
    // XUNTONG present. A confirming Flock name => LIKELY; a nameless XUNTONG advert => POSSIBLE; a XUNTONG device
    // carrying some OTHER name is not flagged (mirrors Marauder's "Penguin-name OR nameless" gate — avoids
    // flagging unrelated XUNTONG-chipset gear that advertises its own product name).
    if (flock_name_match(d->name, d->name_len)) {
        return LXVEOS_BLE_FLOCK_LIKELY;
    }
    if (d->name_len == 0) {
        return LXVEOS_BLE_FLOCK_POSSIBLE;
    }
    return LXVEOS_BLE_FLOCK_NONE;
}

const char *lxveos_ble_flock_str(uint8_t confidence)
{
    switch (confidence) {
    case LXVEOS_BLE_FLOCK_LIKELY:   return "likely";
    case LXVEOS_BLE_FLOCK_POSSIBLE: return "possible";
    default:                        return NULL;
    }
}

uint8_t lxveos_ble_surveil_flags(const lxveos_ble_dev_t *d)
{
    if (d == NULL) {
        return LXVEOS_SURVEIL_NONE;
    }
    // Fold every passive detector into one bitmask. d->tracker was set by the scan-time classifier; the rest
    // are the pure helpers above. Order-independent — a device could in principle match more than one bit.
    uint8_t f = LXVEOS_SURVEIL_NONE;
    if (d->tracker != LXVEOS_BLE_TRACKER_NONE) {
        f |= LXVEOS_SURVEIL_TRACKER;
    }
    if (lxveos_ble_flock_confidence(d) != LXVEOS_BLE_FLOCK_NONE) {
        f |= LXVEOS_SURVEIL_FLOCK;
    }
    if (lxveos_ble_is_meta(d)) {
        f |= LXVEOS_SURVEIL_META;
    }
    if (lxveos_ble_flipper_color(d) != NULL) {
        f |= LXVEOS_SURVEIL_FLIPPER;
    }
    if (lxveos_ble_is_skimmer(d)) {
        f |= LXVEOS_SURVEIL_SKIMMER;
    }
    return f;
}

const char *lxveos_ble_surveil_str(uint8_t category_bit)
{
    switch (category_bit) {
    case LXVEOS_SURVEIL_TRACKER: return "tracker";
    case LXVEOS_SURVEIL_FLOCK:   return "flock-cam";
    case LXVEOS_SURVEIL_META:    return "meta-glasses";
    case LXVEOS_SURVEIL_FLIPPER: return "flipper";
    case LXVEOS_SURVEIL_SKIMMER: return "skimmer?";
    default:                     return NULL;
    }
}
