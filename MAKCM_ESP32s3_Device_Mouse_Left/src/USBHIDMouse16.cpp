#include "USBHIDMouse16.h"

#define MOUSE16_REPORT_ID 0x02

static const uint8_t mouse16_report_descriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, MOUSE16_REPORT_ID, // Report ID
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x01,        //     Input (Constant) - padding
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x01, 0x80,  //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x95, 0x02,        //     Report Count (2)
    0x75, 0x10,        //     Report Size (16)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x08,        //     Report Size (8)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x08,        //     Report Size (8)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0               // End Collection
};

USBHIDMouse16::USBHIDMouse16() : hid(), _buttons(0) {
}

void USBHIDMouse16::begin() {
    hid.addDevice(this, sizeof(mouse16_report_descriptor));
}

void USBHIDMouse16::end() {
}

uint16_t USBHIDMouse16::_onGetDescriptor(uint8_t* buffer) {
    memcpy(buffer, mouse16_report_descriptor, sizeof(mouse16_report_descriptor));
    return sizeof(mouse16_report_descriptor);
}

void USBHIDMouse16::press(uint8_t b) {
    _buttons |= b;
    move(0, 0);
}

void USBHIDMouse16::release(uint8_t b) {
    _buttons &= ~b;
    move(0, 0);
}

void USBHIDMouse16::click(uint8_t b) {
    press(b);
    release(b);
}

bool USBHIDMouse16::isPressed(uint8_t b) {
    return (_buttons & b) != 0;
}

void USBHIDMouse16::move(int16_t x, int16_t y, int8_t wheel, int8_t pan) {
    mouse_report_16_t report = {
        .buttons = _buttons,
        .x = x,
        .y = y,
        .wheel = wheel,
        .pan = pan
    };
    hid.SendReport(MOUSE16_REPORT_ID, &report, sizeof(report));
}
