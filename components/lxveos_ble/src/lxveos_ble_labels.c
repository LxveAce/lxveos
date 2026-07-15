// lxveos_ble_labels — pure BLE value->label helpers split out of lxveos_ble.c so they can be host-unit-
// tested (tests/host_c/test_ble_labels.c) without NimBLE: the company/service name lookups, the item-tracker
// label, and the GAP appearance label. The NimBLE-coupled bits (addr_type_str, the advert classifier and its
// ble_hs_adv_fields parsing) stay in lxveos_ble.c. Behaviour-preserving extraction — the tables and the
// mappings are unchanged, so scan/tracker output is identical.
#include "lxveos_ble.h"
#include "lxveos_ble_internal.h"

#include <stdio.h>

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
