#pragma once

#include "USBHID.h"

typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t  wheel;
    int8_t  pan;
} mouse_report_16_t;

class USBHIDMouse16 : public USBHIDDevice {
public:
    USBHIDMouse16();
    void begin();
    void end();
    void press(uint8_t b = MOUSE_BUTTON_LEFT);
    void release(uint8_t b = MOUSE_BUTTON_LEFT);
    void click(uint8_t b = MOUSE_BUTTON_LEFT);
    bool isPressed(uint8_t b = MOUSE_BUTTON_LEFT);
    void move(int16_t x, int16_t y, int8_t wheel = 0, int8_t pan = 0);

    uint16_t _onGetDescriptor(uint8_t* buffer);

private:
    uint8_t _buttons;
    USBHID hid;
};
