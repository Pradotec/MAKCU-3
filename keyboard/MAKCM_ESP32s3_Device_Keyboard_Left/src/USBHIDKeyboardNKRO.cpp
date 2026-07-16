#include "USBHIDKeyboardNKRO.h"
#include <cstring>

static const uint8_t nkro_report_descriptor[] = {
    // ================= Report ID 1: NKRO Keyboard =================
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, NKRO_KB_REPORT_ID, //   Report ID (1)

    // Modifier byte (usages 0xE0-0xE7)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // LED output report (5 LEDs + 3 bits padding) for host Caps/Num/Scroll
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x91, 0x02,        //   Output (Data, Variable, Absolute)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Constant)

    // NKRO bitmap: keycodes 0x00-0x9F (160 bits = 20 bytes)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x9F,        //   Usage Maximum (0x9F)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0xA0,        //   Report Count (160)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection

    // ================= Report ID 2: Consumer Control =================
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, NKRO_CC_REPORT_ID, //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x10,        //   Report Count (16)
    0x09, 0xCD,        //   Usage (Play/Pause)         bit 0
    0x09, 0xB5,        //   Usage (Scan Next Track)    bit 1
    0x09, 0xB6,        //   Usage (Scan Previous)      bit 2
    0x09, 0xB7,        //   Usage (Stop)               bit 3
    0x09, 0xE2,        //   Usage (Mute)               bit 4
    0x09, 0xE9,        //   Usage (Volume Increment)   bit 5
    0x09, 0xEA,        //   Usage (Volume Decrement)   bit 6
    0x09, 0xB3,        //   Usage (Fast Forward)       bit 7
    0x09, 0xB4,        //   Usage (Rewind)             bit 8
    0x09, 0xB8,        //   Usage (Eject)              bit 9
    0x0A, 0x8A, 0x01,  //   Usage (AL Email Reader)    bit 10
    0x0A, 0x92, 0x01,  //   Usage (AL Calculator)      bit 11
    0x0A, 0x23, 0x02,  //   Usage (AC Home)            bit 12
    0x0A, 0x24, 0x02,  //   Usage (AC Back)            bit 13
    0x0A, 0x25, 0x02,  //   Usage (AC Forward)         bit 14
    0x0A, 0x21, 0x02,  //   Usage (AC Search)          bit 15
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0               // End Collection
};

USBHIDKeyboardNKRO::USBHIDKeyboardNKRO()
    : _ptMods(0), _ptConsumer(0), _simMods(0), _simConsumer(0), hid() {
    memset(_ptMap, 0, sizeof(_ptMap));
    memset(_simMap, 0, sizeof(_simMap));
}

void USBHIDKeyboardNKRO::begin() {
    hid.addDevice(this, sizeof(nkro_report_descriptor));
    // Mandatory: creates the (shared, file-scope) TX semaphore/mutex that
    // USBHID::SendReport requires. Without it every report is dropped with
    // "TX Semaphore is NULL". begin() is idempotent (guards on NULL).
    hid.begin();
}

void USBHIDKeyboardNKRO::end() {
}

uint16_t USBHIDKeyboardNKRO::_onGetDescriptor(uint8_t *buffer) {
    memcpy(buffer, nkro_report_descriptor, sizeof(nkro_report_descriptor));
    return sizeof(nkro_report_descriptor);
}

void USBHIDKeyboardNKRO::sendKeyReport() {
    uint8_t buf[NKRO_KB_PAYLOAD];
    buf[0] = _ptMods | _simMods;
    for (int i = 0; i < NKRO_BITMAP_BYTES; i++) {
        buf[1 + i] = _ptMap[i] | _simMap[i];
    }
    hid.SendReport(NKRO_KB_REPORT_ID, buf, sizeof(buf));
}

void USBHIDKeyboardNKRO::sendConsumerReport() {
    uint16_t bits = _ptConsumer | _simConsumer;
    uint8_t buf[2] = { (uint8_t)(bits & 0xFF), (uint8_t)((bits >> 8) & 0xFF) };
    hid.SendReport(NKRO_CC_REPORT_ID, buf, sizeof(buf));
}

void USBHIDKeyboardNKRO::setPassthrough(uint8_t modifiers, const uint8_t *bitmap) {
    _ptMods = modifiers;
    memcpy(_ptMap, bitmap, NKRO_BITMAP_BYTES);
    sendKeyReport();
}

void USBHIDKeyboardNKRO::setPassthroughConsumer(uint16_t bits) {
    _ptConsumer = bits;
    sendConsumerReport();
}

void USBHIDKeyboardNKRO::pressKey(uint8_t keycode) {
    if (keycode >= 0xE0 && keycode <= 0xE7) {
        _simMods |= (1 << (keycode - 0xE0));
    } else if (keycode <= NKRO_KEY_MAX) {
        _simMap[keycode >> 3] |= (1 << (keycode & 7));
    } else {
        return;
    }
    sendKeyReport();
}

void USBHIDKeyboardNKRO::releaseKey(uint8_t keycode) {
    if (keycode >= 0xE0 && keycode <= 0xE7) {
        _simMods &= ~(1 << (keycode - 0xE0));
    } else if (keycode <= NKRO_KEY_MAX) {
        _simMap[keycode >> 3] &= ~(1 << (keycode & 7));
    } else {
        return;
    }
    sendKeyReport();
}

void USBHIDKeyboardNKRO::releaseAllKeys() {
    _simMods = 0;
    memset(_simMap, 0, sizeof(_simMap));
    sendKeyReport();
}

void USBHIDKeyboardNKRO::pressConsumer(uint8_t bit) {
    if (bit > 15) return;
    _simConsumer |= (1 << bit);
    sendConsumerReport();
}

void USBHIDKeyboardNKRO::releaseConsumer(uint8_t bit) {
    if (bit > 15) return;
    _simConsumer &= ~(1 << bit);
    sendConsumerReport();
}

void USBHIDKeyboardNKRO::setConsumer(uint16_t bits) {
    _simConsumer = bits;
    sendConsumerReport();
}

bool USBHIDKeyboardNKRO::isKeyDown(uint8_t keycode) {
    if (keycode >= 0xE0 && keycode <= 0xE7) {
        return ((_ptMods | _simMods) & (1 << (keycode - 0xE0))) != 0;
    } else if (keycode <= NKRO_KEY_MAX) {
        return ((_ptMap[keycode >> 3] | _simMap[keycode >> 3]) & (1 << (keycode & 7))) != 0;
    }
    return false;
}
