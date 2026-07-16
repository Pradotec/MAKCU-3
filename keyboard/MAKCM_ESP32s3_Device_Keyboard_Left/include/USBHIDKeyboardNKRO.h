#pragma once

#include "USBHID.h"

// Full N-Key Rollover keyboard with a bitmap of key states plus a
// consumer-control (media key) report. Presents to the PC as a single
// HID interface carrying two report IDs.
//
//   Report ID 1 (Keyboard NKRO): 1 modifier byte + 20-byte key bitmap.
//       Bit k of the bitmap = HID usage code k (Keyboard/Keypad page 0x07),
//       covering usages 0x00-0x9F (all standard, keypad, international and
//       language keys). Modifiers (usage 0xE0-0xE7) live in the modifier byte.
//
//   Report ID 2 (Consumer Control): 16-bit bitmap of common media keys.
//
// State is tracked in two independent domains that are OR-combined before
// every report is sent:
//   - passthrough: mirrors the physical keyboard read by the Right MCU
//   - simulated:   injected keys from an external controller over Serial0
// This lets a physically-held key and an injected key coexist.

#define NKRO_BITMAP_BYTES   20      // covers keycodes 0x00 - 0x9F
#define NKRO_KEY_MAX        0x9F
#define NKRO_KB_REPORT_ID   0x01
#define NKRO_CC_REPORT_ID   0x02
#define NKRO_KB_PAYLOAD     (1 + NKRO_BITMAP_BYTES)   // modifiers + bitmap

// Consumer-control bit assignments (index into the 16-bit media bitmap).
enum ConsumerBit {
    CC_PLAY_PAUSE = 0,
    CC_NEXT       = 1,
    CC_PREV       = 2,
    CC_STOP       = 3,
    CC_MUTE       = 4,
    CC_VOL_UP     = 5,
    CC_VOL_DOWN   = 6,
    CC_FAST_FWD   = 7,
    CC_REWIND     = 8,
    CC_EJECT      = 9,
    CC_EMAIL      = 10,
    CC_CALC       = 11,
    CC_AC_HOME    = 12,
    CC_AC_BACK    = 13,
    CC_AC_FORWARD = 14,
    CC_AC_SEARCH  = 15,
};

class USBHIDKeyboardNKRO : public USBHIDDevice {
public:
    USBHIDKeyboardNKRO();
    void begin();
    void end();

    // --- Passthrough domain (physical keyboard mirrored from Right MCU) ---
    void setPassthrough(uint8_t modifiers, const uint8_t *bitmap);
    void setPassthroughConsumer(uint16_t bits);

    // --- Simulated domain (injected by an external controller) ---
    void pressKey(uint8_t keycode);
    void releaseKey(uint8_t keycode);
    void releaseAllKeys();
    void pressConsumer(uint8_t bit);
    void releaseConsumer(uint8_t bit);
    void setConsumer(uint16_t bits);    // set simulated consumer bitmap wholesale

    // --- Queries (union of both domains) ---
    bool isKeyDown(uint8_t keycode);

    uint16_t _onGetDescriptor(uint8_t *buffer);

private:
    uint8_t  _ptMods;
    uint8_t  _ptMap[NKRO_BITMAP_BYTES];
    uint16_t _ptConsumer;

    uint8_t  _simMods;
    uint8_t  _simMap[NKRO_BITMAP_BYTES];
    uint16_t _simConsumer;

    USBHID hid;

    void sendKeyReport();
    void sendConsumerReport();
};
