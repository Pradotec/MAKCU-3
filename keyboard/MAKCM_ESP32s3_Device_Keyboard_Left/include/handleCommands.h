#pragma once

#include "InitSettings.h"
#include <Arduino.h>
#include <USB.h>
#include "USBHIDKeyboardNKRO.h"
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>

// Extern variables
extern USBHIDKeyboardNKRO Keyboard;
extern TaskHandle_t ledFlashTaskHandle;
extern const char *commandQueue[];
extern int currentCommandIndex;
extern bool usbReady;

// Buffer lengths
#define MAX_SERIAL0_COMMAND_LENGTH 100
#define MAX_SERIAL1_COMMAND_LENGTH 600

// Command buffers
extern char serial0Buffer[MAX_SERIAL0_COMMAND_LENGTH];
extern char serial1Buffer[MAX_SERIAL1_COMMAND_LENGTH];

extern std::atomic<bool> serial0Locked;

// Core / infrastructure
void handleDebugcommand(const char *command);
void serial1RX();
void serial0RX();
void notifyLedFlashTask();

// Keyboard injection handlers (external controller over Serial0)
void handleKbPress(const char *command);       // kb.press(code)
void handleKbRelease(const char *command);     // kb.release(code)
void handleKbReleaseAll(const char *command);  // kb.releaseall
void handleKbTap(const char *command);         // kb.tap(code)
void handleKbType(const char *command);        // kb.type(text)
void handleKbIsDown(const char *command);      // kb.isdown(code)
void handleKbMediaTap(const char *command);    // kb.mtap(bit)
void handleKbMediaSet(const char *command);    // kb.mset(bits)

void handleUsbHello(const char *command);
void handleUsbGoodbye(const char *command);
void handleNoDevice(const char *command);
void handleDebug(const char* command);
void handleSerial0Speed(const char* command);
void handleEspLog(const char* command);
void sendNextCommand();
void processCommand(const char *command);

// Extern functions for JSON data handling
extern void receiveDeviceInfo(const char *jsonString);
extern void receiveDescriptorDevice(const char *jsonString);
extern void receiveEndpointDescriptors(const char *jsonString);
extern void receiveInterfaceDescriptors(const char *jsonString);
extern void receiveHidDescriptors(const char *jsonString);
extern void receiveIADescriptors(const char *jsonString);
extern void receiveEndpointData(const char *jsonString);
extern void receiveUnknownDescriptors(const char *jsonString);
extern void receivedescriptorConfiguration(const char *jsonString);

// Command table structure
struct CommandEntry {
    const char *command;
    void (*handler)(const char *);
};
